/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>

#include <cutils/ashmem.h>
#include <cutils/log.h>
#include <cutils/atomic.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#include "gralloc_priv.h"
#include "allocator.h"

#if HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif

/*****************************************************************************/

struct gralloc_context_t {
    alloc_device_t  device;
    /* our private data here */
	int bufferType;
};

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle);

/*****************************************************************************/

int fb_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

static int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

extern int gralloc_lock(gralloc_module_t const* module,
        buffer_handle_t handle, int usage,
        int l, int t, int w, int h,
        void** vaddr);

extern int gralloc_unlock(gralloc_module_t const* module, 
        buffer_handle_t handle);

extern int gralloc_register_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

extern int gralloc_unregister_buffer(gralloc_module_t const* module,
        buffer_handle_t handle);

/*****************************************************************************/

static struct hw_module_methods_t gralloc_module_methods = {
        open: gralloc_device_open
};

struct private_module_t HAL_MODULE_INFO_SYM = {
    base: {
        common: {
            tag: HARDWARE_MODULE_TAG,
            version_major: 1,
            version_minor: 0,
            id: GRALLOC_HARDWARE_MODULE_ID,
            name: "Graphics Memory Allocator Module",
            author: "The Android Open Source Project",
            methods: &gralloc_module_methods
        },
        registerBuffer: gralloc_register_buffer,
        unregisterBuffer: gralloc_unregister_buffer,
        lock: gralloc_lock,
        unlock: gralloc_unlock,
    },
    framebuffer: 0,
    flags: 0,
    numBuffers: 0,
    bufferMask: 0,
    lock: PTHREAD_MUTEX_INITIALIZER,
    currentBuffer: 0,
    pmem_master: -1,
    pmem_master_base: 0,
    master_phys: 0,
    gpu_master: -1,
    gpu_master_base: 0
};

/*****************************************************************************/

static int gralloc_alloc_framebuffer_locked(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);

    // allocate the framebuffer
    if (m->framebuffer == NULL) {
        // initialize the framebuffer, the framebuffer is mapped once
        // and forever.
        int err = mapFrameBufferLocked(m);
        if (err < 0) {
            return err;
        }
    }

    const uint32_t bufferMask = m->bufferMask;
    const uint32_t numBuffers = m->numBuffers;
    const size_t bufferSize = m->finfo.line_length * m->info.yres;
    if (numBuffers == 1) {
        // If we have only one buffer, we never use page-flipping. Instead,
        // we return a regular buffer which will be memcpy'ed to the main
        // screen when post is called.
        int newUsage = (usage & ~GRALLOC_USAGE_HW_FB) | GRALLOC_USAGE_HW_2D;
        return gralloc_alloc_buffer(dev, bufferSize, newUsage, pHandle);
    }

    if (bufferMask >= ((1LU<<numBuffers)-1)) {
        // We ran out of buffers.
        return -ENOMEM;
    }

    // create a "fake" handles for it
    intptr_t vaddr = intptr_t(m->framebuffer->base);
    private_handle_t* hnd = new private_handle_t(dup(m->framebuffer->fd), size,
            private_handle_t::PRIV_FLAGS_USES_PMEM |
            private_handle_t::PRIV_FLAGS_FRAMEBUFFER, BUFFER_TYPE_FB);

    // find a free slot
    for (uint32_t i=0 ; i<numBuffers ; i++) {
        if ((bufferMask & (1LU<<i)) == 0) {
            m->bufferMask |= (1LU<<i);
            break;
        }
        vaddr += bufferSize;
    }

    hnd->base = vaddr;
    hnd->offset = vaddr - intptr_t(m->framebuffer->base);
    hnd->phys = intptr_t(m->framebuffer->phys) + hnd->offset;
    *pHandle = hnd;

    return 0;
}

static int gralloc_alloc_framebuffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    private_module_t* m = reinterpret_cast<private_module_t*>(
            dev->common.module);
    pthread_mutex_lock(&m->lock);
    int err = gralloc_alloc_framebuffer_locked(dev, size, usage, pHandle);
    pthread_mutex_unlock(&m->lock);
    return err;
}

static SimpleBestFitAllocator sAllocator(10*1024*1024);
static SimpleBestFitAllocator sGPUAllocator(3*1024*1024);

static int init_pmem_area(private_module_t* m, int type)
{
    int err = 0;
    int master_fd = -1;
    size_t master_heap_size;
    if(type == BUFFER_TYPE_GPU0)
    {
        master_fd = open("/dev/pmem_gpu0", O_RDWR, 0);
        master_heap_size = sGPUAllocator.size();
    }
    else if(type == BUFFER_TYPE_GPU1)
    {
        master_fd = open("/dev/pmem_gpu1", O_RDWR, 0);
        master_heap_size = sGPUAllocator.size();
    }
    else if (type == BUFFER_TYPE_PMEM)
    {
        master_fd = open("/dev/pmem", O_RDWR, 0);
        master_heap_size = sAllocator.size();
    }
    
    if (master_fd >= 0) {
        void* base = mmap(0, master_heap_size,
                PROT_READ|PROT_WRITE, MAP_SHARED, master_fd, 0);
        if (base == MAP_FAILED) {
            LOGE("Enter init_pmem_area error: %d", -errno);
            err = -errno;
            base = 0;
            close(master_fd);
            master_fd = -1;
        }
        if(type == BUFFER_TYPE_PMEM){
        	m->pmem_master = master_fd;
        	m->pmem_master_base = base;
        }
        else
    	{
            m->gpu_master = master_fd;
            m->gpu_master_base = base;
    		pmem_region region;
        	err = ioctl(m->gpu_master, PMEM_GET_PHYS, &region);
        	if(err < 0)
        	{
        		LOGE("init pmem: master ioctl failed %d", -errno);
        	}
        	else
        	{
               	m->master_phys = (unsigned long)region.offset;
        	}
        }
    } else {
        err = -errno;
    }
    return err;
}

static int gralloc_alloc_buffer(alloc_device_t* dev,
        size_t size, int usage, buffer_handle_t* pHandle)
{
    int err = 0;
    int flags = 0;

    int fd = -1;
    void* base = 0;
    int offset = 0;
    int lockState = 0;
    
    private_module_t* m = reinterpret_cast<private_module_t*>(
                dev->common.module);

    gralloc_context_t *context = (gralloc_context_t *) dev;
    int bufferType;

    size = roundUpToPageSize(size);
    
    if (usage & (GRALLOC_USAGE_HW_2D | GRALLOC_USAGE_HW_RENDER)) {
        flags |= private_handle_t::PRIV_FLAGS_USES_PMEM;
        bufferType = context->bufferType;
    }
    else if (usage & GRALLOC_USAGE_HW_TEXTURE) {
        // enable pmem in that case, so our software GL can fallback to
        // the copybit module.
        flags |= private_handle_t::PRIV_FLAGS_USES_PMEM;
        bufferType = BUFFER_TYPE_PMEM;
    }

    int phys = 0;
    if ((flags & private_handle_t::PRIV_FLAGS_USES_PMEM) == 0) {
try_ashmem:
        fd = ashmem_create_region("Buffer", size);
        if (fd < 0) {
            LOGE("couldn't create ashmem (%s)", strerror(-errno));
            err = -errno;
        }
    } else {
       
        int master_fd = -1;    
        if(bufferType == BUFFER_TYPE_PMEM)
        {
            master_fd = m->pmem_master;
        }
        else      
        {
            master_fd = m->gpu_master;
        }

        pthread_mutex_lock(&m->lock);
        if (master_fd == -1) {
            err = init_pmem_area(m, bufferType);
        }
        pthread_mutex_unlock(&m->lock);
        
        if(bufferType == BUFFER_TYPE_PMEM)
        {
            master_fd = m->pmem_master;
        }
        else
        {
            master_fd = m->gpu_master;
        }
          
        if (master_fd >= 0) {
            // PMEM buffers are always mmapped
            if(bufferType == BUFFER_TYPE_PMEM)
            {
                base = m->pmem_master_base;
                offset = sAllocator.allocate(size);
            }
            else
            {
               base = m->gpu_master_base;
               offset = sGPUAllocator.allocate(size);
            }
            
            lockState |= private_handle_t::LOCK_STATE_MAPPED;

            if (offset < 0) {
                err = -ENOMEM;
            } else {
                if(bufferType == BUFFER_TYPE_GPU0)
                    fd = open("/dev/pmem_gpu0", O_RDWR, 0);
                else if(bufferType == BUFFER_TYPE_GPU1)
                    fd = open("/dev/pmem_gpu1", O_RDWR, 0);
                else if (bufferType == BUFFER_TYPE_PMEM)
                    fd = open("/dev/pmem", O_RDWR, 0);

                err = ioctl(fd, PMEM_CONNECT, master_fd);
                if (err < 0) {
                    err = -errno;
                } else {
                    struct pmem_region sub = { offset, size };
                    err = ioctl(fd, PMEM_MAP, &sub);
                }
                
                if (err < 0) {
                    close(fd);
                    if(bufferType == BUFFER_TYPE_PMEM)
                    	sAllocator.deallocate(offset);
                    else
                        sGPUAllocator.deallocate(offset);
                    fd = -1;
                }
                //LOGD_IF(!err, "allocating pmem size=%d, offset=%d", size, offset);
            }
        } else {
            if ((usage & GRALLOC_USAGE_HW_2D) == 0) {
                // the caller didn't request PMEM, so we can try something else
                flags &= ~private_handle_t::PRIV_FLAGS_USES_PMEM;
                err = 0;
                goto try_ashmem;
            } else {
                LOGE("couldn't open pmem (%s)", strerror(-errno));
            }
        } 
    }

    if (err == 0) {
        private_handle_t* hnd = new private_handle_t(fd, size, flags, bufferType);
        hnd->offset = offset;
        hnd->base = int(base)+offset;
        hnd->lockState = lockState;
        if(bufferType == BUFFER_TYPE_GPU1)
        	hnd->phys = m->master_phys + offset;
        else
                hnd->phys = 0;
        *pHandle = hnd;
    }
    
    LOGE_IF(err, "gralloc failed err=%s", strerror(-err));
    
    return err;
}

/*****************************************************************************/

static int gralloc_alloc(alloc_device_t* dev,
        int w, int h, int format, int usage,
        buffer_handle_t* pHandle, int* pStride)
{
    if (!pHandle || !pStride)
        return -EINVAL;

    int align = 4;
    int bpp = 0;
    switch (format) {
        case HAL_PIXEL_FORMAT_RGBA_8888:
        case HAL_PIXEL_FORMAT_BGRA_8888:
            bpp = 4;
            break;
        case HAL_PIXEL_FORMAT_RGB_565:
        case HAL_PIXEL_FORMAT_RGBA_5551:
        case HAL_PIXEL_FORMAT_RGBA_4444:
            bpp = 2;
            break;
        default:
            return -EINVAL;
    }

    size_t bpr = (w*bpp + (align-1)) & ~(align-1);
    size_t size = bpr * h;
    size_t stride = bpr / bpp;

    int err;
    if (usage & GRALLOC_USAGE_HW_FB) {
        err = gralloc_alloc_framebuffer(dev, size, usage, pHandle);
    } else {
        err = gralloc_alloc_buffer(dev, size, usage, pHandle);
    }
    if (err < 0) {
        return err;
    }

    *pStride = stride;
    return 0;
}

static int gralloc_free(alloc_device_t* dev,
        buffer_handle_t handle)
{
    if (private_handle_t::validate(handle) < 0)
        return -EINVAL;

    private_handle_t const* hnd = reinterpret_cast<private_handle_t const*>(handle);
    if (hnd->flags & private_handle_t::PRIV_FLAGS_FRAMEBUFFER) {
        // free this buffer
        private_module_t* m = reinterpret_cast<private_module_t*>(
                dev->common.module);
        const size_t bufferSize = m->finfo.line_length * m->info.yres;
        int index = (hnd->base - m->framebuffer->base) / bufferSize;
        m->bufferMask &= ~(1<<index); 
    } else if (true || hnd->flags & private_handle_t::PRIV_FLAGS_USES_PMEM) {
        if (hnd->fd >= 0) {
            if(hnd->bufferType == BUFFER_TYPE_PMEM){
                sAllocator.deallocate(hnd->offset);
                memset((void *)hnd->base, 0, hnd->size);
            }
            else {
                  sGPUAllocator.deallocate(hnd->offset);
                memset((void *)hnd->base, 0, hnd->size);
            }
       }
    }

    gralloc_module_t* m = reinterpret_cast<gralloc_module_t*>(
            dev->common.module);
    gralloc_unregister_buffer(m, handle);
    
    close(hnd->fd);
    delete hnd;
    return 0;
}

/*****************************************************************************/

static int gralloc_close(struct hw_device_t *dev)
{
    gralloc_context_t* ctx = reinterpret_cast<gralloc_context_t*>(dev);
    if (ctx) {
        /* TODO: keep a list of all buffer_handle_t created, and free them
         * all here.
         */
        free(ctx);
    }
    return 0;
}

int gralloc_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (!strcmp(name, GRALLOC_HARDWARE_GPU0)) {
        gralloc_context_t *dev;
        dev = (gralloc_context_t*)malloc(sizeof(*dev));

        /* initialize our state here */
        memset(dev, 0, sizeof(*dev));

        /* initialize the procs */
        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version = 0;
        dev->device.common.module = const_cast<hw_module_t*>(module);
        dev->device.common.close = gralloc_close;

        dev->device.alloc   = gralloc_alloc;
        dev->device.free    = gralloc_free;
        dev->bufferType = BUFFER_TYPE_GPU1;
        *device = &dev->device.common;
        status = 0;
    } else {
        status = fb_device_open(module, name, device);
    }
    return status;
}
