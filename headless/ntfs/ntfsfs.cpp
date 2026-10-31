#include "ntfsfs.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <set>

#include "../diagnostics.h"

extern "C"
{
#include "bdev.h"
#include "ntfs.h"
#include "part.h"
}

extern "C" int sceKernelGetdents(int, char *, int);

namespace
{
void ntfs_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void ntfs_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    Eden::Report("ntfs", line);
}

// The names in /dev, read the console's way (the C library's opendir is the platform's, which this app leaves alone).
std::vector<std::string> dev_names()
{
    std::vector<std::string> names;
    const int fd = open("/dev", O_RDONLY | O_DIRECTORY);
    if (fd < 0)
    {
        ntfs_log("cannot read /dev: errno %d", errno);
        return names;
    }
    std::vector<char> buffer(65536);
    for (;;)
    {
        const int count = sceKernelGetdents(fd, buffer.data(), static_cast<int>(buffer.size()));
        if (count <= 0 || count > static_cast<int>(buffer.size()))
            break;
        for (size_t offset = 0; offset + offsetof(dirent, d_name) < static_cast<size_t>(count);)
        {
            uint16_t length;
            memcpy(&length, buffer.data() + offset + offsetof(dirent, d_reclen), sizeof(length));
            if (length <= offsetof(dirent, d_name) || offset + length > static_cast<size_t>(count))
                break;
            const char *name = buffer.data() + offset + offsetof(dirent, d_name);
            names.emplace_back(name, strnlen(name, length - offsetof(dirent, d_name)));
            offset += length;
        }
    }
    close(fd);
    return names;
}
} // namespace

namespace
{
struct Vol
{
    std::string name;
    std::string dev;
    ntfs_vol_t *v = nullptr;
    uint64_t serial = 0;
    bool gone = false;
    bool dirty = false;
};

std::mutex g_mutex;
std::vector<Vol *> g_vols;
std::atomic<bool> g_lost{false};

bool normalize(const char *path, std::string &out)
{
    std::vector<std::string> parts;
    std::string part;
    for (const char *p = path;; ++p)
    {
        if (*p == '/' || !*p)
        {
            if (part == "..")
            {
                if (parts.empty())
                    return false;
                parts.pop_back();
            }
            else if (!part.empty() && part != ".")
                parts.push_back(part);
            part.clear();
            if (!*p)
                break;
        }
        else
            part += *p;
    }
    out.clear();
    for (const std::string &s : parts)
        out += "/" + s;
    if (out.empty())
        out = "/";
    return true;
}

struct Where
{
    Vol *vol = nullptr;
    std::string rel;
    bool root = false;
};

int resolve(const char *path, Where &w)
{
    std::string p;
    if (!normalize(path, p) || p.compare(0, 5, "/ntfs") != 0 || (p.size() > 5 && p[5] != '/'))
        return ENOENT;
    if (p.size() == 5)
    {
        w.root = true;
        return 0;
    }
    const size_t to = p.find('/', 6);
    const std::string name = p.substr(6, to == std::string::npos ? std::string::npos : to - 6);
    std::lock_guard<std::mutex> g(g_mutex);
    for (Vol *v : g_vols)
        if (!v->gone && v->name == name)
        {
            w.vol = v;
            w.rel = to == std::string::npos ? "/" : p.substr(to);
            return 0;
        }
    return ENOENT;
}

int error_of(int rc)
{
    return rc < 0 ? (-rc == EIO ? EIO : -rc) : rc;
}

void fill(struct stat *st, const ntfs_stat_t &ns)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = static_cast<mode_t>(ns.is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444));
    st->st_nlink = 1;
    st->st_size = static_cast<off_t>(ns.size);
    st->st_mtime = static_cast<time_t>(ns.mtime);
    st->st_atime = static_cast<time_t>(ns.atime);
    st->st_ctime = static_cast<time_t>(ns.crtime);
    st->st_blksize = 4096;
    st->st_blocks = static_cast<blkcnt_t>((ns.size + 511) / 512);
    st->st_ino = static_cast<ino_t>(ns.inode);
}

int lookup(const Where &w, uint64_t *inode, ntfs_stat_t *ns)
{
    int rc = ntfs_lookup(w.vol->v, w.rel.c_str(), inode);
    if (rc == 0)
        rc = ntfs_stat(w.vol->v, *inode, ns);
    return rc < 0 ? error_of(rc) : 0;
}

std::string clean_name(const char *label, uint64_t serial, int index)
{
    std::string n = label;
    for (char &c : n)
        if (c == '/' || c == '\\')
            c = '_';
    if (n.empty())
        n = "ntfs" + std::to_string(index);
    std::lock_guard<std::mutex> g(g_mutex);
    std::string unique = n;
    for (int k = 2;; ++k)
    {
        bool taken = false;
        for (Vol *v : g_vols)
            if (!v->gone && v->name == unique)
                taken = true;
        if (!taken)
            return unique;
        unique = n + "-" + std::to_string(k);
    }
    (void)serial;
}

bool known(uint64_t serial)
{
    std::lock_guard<std::mutex> g(g_mutex);
    for (Vol *v : g_vols)
        if (!v->gone && v->serial == serial)
            return true;
    return false;
}

void add_volume(bdev_t *dev, const std::string &devname)
{
    ntfs_vol_t *nv = nullptr;
    char err[128] = "";
    const int rc = ntfs_mount(dev, &nv, err, sizeof(err));
    if (rc != 0)
    {
        ntfs_log("ntfs: %s: %s", devname.c_str(), err);
        bdev_close(dev);
        return;
    }
    ntfs_info_t info;
    ntfs_get_info(nv, &info);
    if (known(info.serial))
    {
        ntfs_umount(nv);
        return;
    }
    Vol *v = new Vol();
    v->v = nv;
    v->dev = devname;
    v->serial = info.serial;
    v->dirty = info.dirty != 0;
    int index;
    {
        std::lock_guard<std::mutex> g(g_mutex);
        index = static_cast<int>(g_vols.size());
    }
    v->name = clean_name(info.label, info.serial, index);
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_vols.push_back(v);
    }
    ntfs_log("ntfs: %s mounted from %s as /ntfs/%s (%llu MB%s)", info.label, devname.c_str(), v->name.c_str(),
                       static_cast<unsigned long long>(info.total_bytes >> 20), v->dirty ? ", not cleanly unmounted" : "");
}

void scan_device(const std::string &name)
{
    char path[64];
    snprintf(path, sizeof(path), "/dev/%s", name.c_str());
    bdev_t *b = bdev_open(path);
    if (!b)
    {
        ntfs_log("%s cannot be opened: errno %d", path, errno);
        return;
    }
    if (part_is_ntfs(b))
    {
        add_volume(b, name);
        return;
    }
    part_t parts[PART_MAX];
    const int n = part_scan(b, parts, PART_MAX);
    for (int i = 0; i < n; ++i)
    {
        if (strcmp(parts[i].scheme, "none") == 0)
            continue;
        bdev_t *s = bdev_slice(b, parts[i].offset, parts[i].size);
        if (!s)
            continue;
        if (part_is_ntfs(s))
            add_volume(s, name + "#" + std::to_string(parts[i].index));
        else
            bdev_close(s);
    }
    bdev_close(b);
}
} // namespace

struct NtfsFile
{
    ntfs_file_t *f = nullptr;
};

extern "C" int ntfsfs_scan(void)
{
    std::vector<std::string> devs;
    std::set<std::string> present;
    for (const std::string &n : dev_names())
    {
        if (n.compare(0, 2, "da") != 0 || n.size() < 3 || n[2] < '0' || n[2] > '9' || n.find('.') != std::string::npos)
            continue;
        devs.push_back(n);
    }
    std::sort(devs.begin(), devs.end());
    for (const std::string &d : devs)
        present.insert(d);
    {
        std::lock_guard<std::mutex> g(g_mutex);
        for (Vol *v : g_vols)
        {
            const std::string base = v->dev.substr(0, v->dev.find('#'));
            if (!v->gone && !present.count(base))
            {
                v->gone = true;
                g_lost = true;
                ntfs_log("ntfs: /ntfs/%s is gone", v->name.c_str());
            }
        }
    }
    for (const std::string &d : devs)
        scan_device(d);
    std::lock_guard<std::mutex> g(g_mutex);
    int count = 0;
    for (Vol *v : g_vols)
        count += v->gone ? 0 : 1;
    return count;
}

extern "C" int ntfsfs_mount_points(char out[][NTFSFS_PATH_MAX], int max)
{
    std::lock_guard<std::mutex> g(g_mutex);
    int n = 0;
    for (Vol *v : g_vols)
        if (!v->gone && n < max)
            snprintf(out[n++], NTFSFS_PATH_MAX, "/ntfs/%s", v->name.c_str());
    return n;
}

extern "C" bool ntfsfs_connection_lost(void)
{
    return g_lost.exchange(false);
}

extern "C" int ntfsfs_dirty_count(void)
{
    std::lock_guard<std::mutex> g(g_mutex);
    int n = 0;
    for (Vol *v : g_vols)
        n += !v->gone && v->dirty ? 1 : 0;
    return n;
}

bool ntfsfs_is_path(const char *path)
{
    if (!path || strncmp(path, "/ntfs", 5) != 0 || (path[5] != '\0' && path[5] != '/'))
        return false;
    std::lock_guard<std::mutex> g(g_mutex);
    return !g_vols.empty();
}

int ntfsfs_stat(const char *path, struct stat *st)
{
    Where w;
    int e = resolve(path, w);
    if (e)
        return e;
    if (w.root)
    {
        ntfs_stat_t ns = {};
        ns.is_dir = 1;
        ns.inode = 1;
        fill(st, ns);
        return 0;
    }
    uint64_t inode;
    ntfs_stat_t ns;
    e = lookup(w, &inode, &ns);
    if (e)
        return e;
    fill(st, ns);
    return 0;
}

NtfsFile *ntfsfs_open(const char *path, uint64_t *size, int *error)
{
    Where w;
    int e = resolve(path, w);
    if (e || w.root)
    {
        *error = e ? e : EISDIR;
        return nullptr;
    }
    uint64_t inode;
    ntfs_stat_t ns;
    e = lookup(w, &inode, &ns);
    if (!e && ns.is_dir)
        e = EISDIR;
    if (e)
    {
        *error = e;
        return nullptr;
    }
    NtfsFile *f = new NtfsFile();
    const int rc = ntfs_open(w.vol->v, inode, &f->f);
    if (rc < 0)
    {
        delete f;
        *error = error_of(rc);
        return nullptr;
    }
    *size = ns.size;
    return f;
}

size_t ntfsfs_pread(NtfsFile *f, void *dst, size_t n, uint64_t pos)
{
    uint8_t *out = static_cast<uint8_t *>(dst);
    size_t got = 0;
    while (got < n)
    {
        const ssize_t r = ntfs_pread(f->f, out + got, n - got, pos + got);
        if (r < 0)
        {
            g_lost = true;
            errno = EIO;
            break;
        }
        if (r == 0)
            break;
        got += static_cast<size_t>(r);
    }
    return got;
}

void ntfsfs_close(NtfsFile *f)
{
    if (!f)
        return;
    ntfs_close(f->f);
    delete f;
}

int ntfsfs_list(const char *path, std::vector<std::pair<std::string, bool>> &entries)
{
    Where w;
    int e = resolve(path, w);
    if (e)
        return e;
    entries.push_back({".", true});
    entries.push_back({"..", true});
    if (w.root)
    {
        std::lock_guard<std::mutex> g(g_mutex);
        for (Vol *v : g_vols)
            if (!v->gone)
                entries.push_back({v->name, true});
        return 0;
    }
    uint64_t inode;
    ntfs_stat_t ns;
    e = lookup(w, &inode, &ns);
    if (e)
        return e;
    if (!ns.is_dir)
        return ENOTDIR;
    const int rc = ntfs_readdir(
        w.vol->v, inode,
        [](void *ctx, const char *name, const ntfs_stat_t *st) -> int
        {
            static_cast<std::vector<std::pair<std::string, bool>> *>(ctx)->push_back({name, st->is_dir != 0});
            return 0;
        },
        &entries);
    return rc < 0 ? error_of(rc) : 0;
}
