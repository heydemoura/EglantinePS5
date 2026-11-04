/* ------------------------------------------------- scattered allocations */

/* EglantinePS5: a buffer the pool has no contiguous range for any more is put
 * together from 2 MiB pieces of direct memory, mapped side by side in the
 * device-memory region. The GPU reads through the same page tables as the CPU,
 * so it needs a contiguous address, not contiguous memory. Without this, a
 * pool cut into 2 MiB holes (the guest memory's pager gives slots back one by
 * one) refuses every 64 MiB VMA block while gigabytes are free.
 * memory->physical is then -2 - the slot of the pieces' table below; every
 * place that reads physical is in this file. RADV_PS5_SCATTER=0 turns it off. */
#define RADV_PS5_SCATTER_SLOTS 8192
struct radv_ps5_scatter {
   int64_t *pieces;
   uint32_t count;
   uint64_t bytes;
};
static struct radv_ps5_scatter radv_ps5_scatters[RADV_PS5_SCATTER_SLOTS];
static simple_mtx_t radv_ps5_scatter_lock = SIMPLE_MTX_INITIALIZER;
static uint64_t radv_ps5_scatter_live_bytes, radv_ps5_scatter_made, radv_ps5_scatter_failed;

static bool radv_ps5_map_fixed(uint8_t *at, uint64_t bytes, int64_t physical);

int sceKernelDebugOutText(int channel, const char *text);

/* To stderr and the kernel log: a crash restart truncates stderr.log. */
static void PRINTFLIKE(1, 2)
radv_ps5_note(const char *format, ...)
{
   char line[256];
   va_list args;
   va_start(args, format);
   vsnprintf(line, sizeof(line), format, args);
   va_end(args);
   fputs(line, stderr);
   sceKernelDebugOutText(0, line);
}

static bool
radv_ps5_scatter_enabled(void)
{
   static int enabled = -1;
   if (enabled < 0) {
      const char *value = getenv("RADV_PS5_SCATTER");
      enabled = !(value && value[0] == '0');
   }
   return enabled;
}

static struct radv_ps5_scatter *
radv_ps5_scatter_of(int64_t physical)
{
   if (physical > -2)
      return NULL;
   const int64_t slot = -2 - physical;
   return slot < RADV_PS5_SCATTER_SLOTS ? &radv_ps5_scatters[slot] : NULL;
}

static uint64_t
radv_ps5_scatter_piece_bytes(const struct radv_ps5_scatter *scatter, uint32_t index)
{
   const uint64_t offset = (uint64_t)index * RADV_PS5_LARGE_BYTES;
   return MIN2(scatter->bytes - offset, RADV_PS5_LARGE_BYTES);
}

/* Backs [va, va + bytes) (taken granules) with pieces; the tag for physical. */
static bool
radv_ps5_scatter_fill(uint8_t *va, uint64_t bytes, int64_t *tag)
{
   if (!radv_ps5_scatter_enabled() || bytes <= RADV_PS5_LARGE_BYTES)
      return false;
   const uint32_t count = (uint32_t)DIV_ROUND_UP(bytes, RADV_PS5_LARGE_BYTES);
   int64_t *pieces = malloc((size_t)count * sizeof(*pieces));
   if (!pieces)
      return false;
   uint32_t done = 0;
   for (; done < count; ++done) {
      const uint64_t offset = (uint64_t)done * RADV_PS5_LARGE_BYTES;
      const uint64_t piece = MIN2(bytes - offset, RADV_PS5_LARGE_BYTES);
      int64_t physical = -1;
      if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), piece,
                                        piece == RADV_PS5_LARGE_BYTES ? RADV_PS5_LARGE_BYTES
                                                                      : PS5_KERNEL_DIRECT_ALIGNMENT,
                                        RADV_PS5_DIRECT_TYPE, &physical) != 0)
         break;
      if (!radv_ps5_map_fixed(va + offset, piece, physical)) {
         sceKernelReleaseDirectMemory(physical, piece);
         break;
      }
      pieces[done] = physical;
   }
   int64_t slot = -1;
   if (done == count) {
      simple_mtx_lock(&radv_ps5_scatter_lock);
      for (int64_t i = 0; i < RADV_PS5_SCATTER_SLOTS; ++i) {
         if (!radv_ps5_scatters[i].pieces) {
            radv_ps5_scatters[i] = (struct radv_ps5_scatter){.pieces = pieces, .count = count, .bytes = bytes};
            slot = i;
            radv_ps5_scatter_live_bytes += bytes;
            ++radv_ps5_scatter_made;
            break;
         }
      }
      simple_mtx_unlock(&radv_ps5_scatter_lock);
   }
   if (slot < 0) {
      /* Every piece mapped so far is a whole 2 MiB one. */
      if (done)
         sceKernelMunmap(va, (uint64_t)done * RADV_PS5_LARGE_BYTES);
      for (uint32_t i = 0; i < done; ++i)
         sceKernelReleaseDirectMemory(pieces[i], RADV_PS5_LARGE_BYTES);
      free(pieces);
      p_atomic_inc(&radv_ps5_scatter_failed);
      static unsigned failures_logged;
      if (failures_logged++ < 16)
         radv_ps5_note("radv/ps5: scattered allocation of %" PRIu64 " MiB failed after %u of %u pieces\n",
                 bytes >> 20, done, count);
      return false;
   }
   static unsigned logged;
   if (logged++ < 64 || (radv_ps5_scatter_made % 64) == 0)
      radv_ps5_note("radv/ps5: scattered allocation of %" PRIu64 " MiB in %u pieces at %p "
                      "(%" PRIu64 " made, %" PRIu64 " MiB live)\n",
              bytes >> 20, count, (void *)va, radv_ps5_scatter_made, radv_ps5_scatter_live_bytes >> 20);
   *tag = -2 - slot;
   return true;
}

static void
radv_ps5_scatter_release(int64_t tag)
{
   struct radv_ps5_scatter *const scatter = radv_ps5_scatter_of(tag);
   if (!scatter)
      return;
   simple_mtx_lock(&radv_ps5_scatter_lock);
   const struct radv_ps5_scatter taken = *scatter;
   *scatter = (struct radv_ps5_scatter){0};
   radv_ps5_scatter_live_bytes -= taken.bytes;
   simple_mtx_unlock(&radv_ps5_scatter_lock);
   for (uint32_t i = 0; i < taken.count; ++i)
      sceKernelReleaseDirectMemory(taken.pieces[i], radv_ps5_scatter_piece_bytes(&taken, i));
   free(taken.pieces);
}

/* Maps [offset, offset + bytes) of a scattered allocation at `at`, piece by piece. */
static bool
radv_ps5_scatter_map(uint8_t *at, uint64_t bytes, int64_t tag, uint64_t offset, uint32_t protection)
{
   const struct radv_ps5_scatter *const scatter = radv_ps5_scatter_of(tag);
   if (!scatter || !scatter->pieces || offset > scatter->bytes || bytes > scatter->bytes - offset)
      return false;
   for (uint64_t done = 0; done < bytes;) {
      const uint64_t position = offset + done;
      const uint32_t index = (uint32_t)(position / RADV_PS5_LARGE_BYTES);
      const uint64_t within = position % RADV_PS5_LARGE_BYTES;
      const uint64_t piece = MIN2(bytes - done, radv_ps5_scatter_piece_bytes(scatter, index) - within);
      void *address = at + done;
      if (sceKernelMapDirectMemory(&address, piece, protection, PS5_KERNEL_MAP_FIXED,
                                   scatter->pieces[index] + (int64_t)within, RADV_PS5_PAGE_BYTES) != 0 ||
          address != at + done)
         return false;
      done += piece;
   }
   return true;
}
