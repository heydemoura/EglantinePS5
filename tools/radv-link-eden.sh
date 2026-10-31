#!/usr/bin/env bash
# Source after setting root to the Eden port. Keep the qualified Eden heap;
# reuse the pinned RADV platform recipe for threads, libc, TLS and unwinding.
eden_radv_link_recipe() {
    local archive=$1
    local reference="$root/../mihawk-vulkan-review"
    local radv_sdk="$reference/.deps/native/ps5-payload-sdk"
    source "$reference/tools/radv-link.sh"
    radv_link_recipe "$reference" "$radv_sdk" "$archive" || return
    # The Eden linker already groups all native archives. lld rejects nested
    # groups, so flatten only the recipe's group delimiters.
    local input
    local inputs=()
    for input in "${radv_link_inputs[@]}"; do
        case "$input" in --start-group|--end-group) ;; *) inputs+=("$input") ;; esac
    done
    radv_link_inputs=("${inputs[@]}")
    # The platform layer wraps fclose and fflush (its open_memstream) and headless/nfs/nfs.cpp wraps them too: the
    # platform's two are kept in a copy of the archive under other names, which nfs.cpp calls for every FILE that
    # is not one of its own.
    local platform_archive="$root/build/libps5platform-nfs.a" i
    mkdir -p "$root/build"
    cp "$radv_sdk/target/lib/libps5platform.a" "$platform_archive"
    "$(command -v llvm-objcopy-18 || command -v llvm-objcopy)" \
        --redefine-sym __wrap_fclose=ps5_memstream_fclose \
        --redefine-sym __wrap_fflush=ps5_memstream_fflush "$platform_archive"
    for i in "${!radv_link_inputs[@]}"; do
        [[ ${radv_link_inputs[i]} == "$radv_sdk/target/lib/libps5platform.a" ]] && radv_link_inputs[i]="$platform_archive"
    done
    local flag
    local retained=()
    for flag in "${radv_link_flags[@]}"; do
        case "$flag" in
            --wrap=malloc|--wrap=calloc|--wrap=realloc|--wrap=free|\
            --wrap=posix_memalign|--wrap=aligned_alloc|--wrap=memalign|\
            --wrap=malloc_usable_size|--wrap=reallocf|--wrap=reallocarray|\
            --wrap=getline|--wrap=getdelim) ;;
            *) retained+=("$flag") ;;
        esac
    done
    # The static Mesa ICD exports radv_GetInstanceProcAddr. Eden's existing
    # static loader uses the standard public name.
    radv_link_flags=("${retained[@]}" --defsym=vkGetInstanceProcAddr=radv_GetInstanceProcAddr
        --undefined=__real_fclose --undefined=__real_fflush)
}
