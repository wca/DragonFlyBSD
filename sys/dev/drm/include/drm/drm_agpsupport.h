#ifndef _DRM_AGPSUPPORT_H_
#define _DRM_AGPSUPPORT_H_

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <dev/agp/agpvar.h>
#include <drm/drmP.h>

typedef struct drm_agp_mem {
	void               *handle;
	unsigned long      bound; /* address */
	int                pages;
	struct drm_agp_mem *prev;
	struct drm_agp_mem *next;
} drm_agp_mem_t;

struct drm_agp_head {
	device_t	   agpdev;
	struct agp_info    agp_info;
	const char         *chipset;
	drm_agp_mem_t      *memory;
	unsigned long mode;
	int enabled;
	int acquired;
	unsigned long base;
	int agp_mtrr;
	int cant_use_aperture;
	unsigned long page_mask;
};

#if __OS_HAS_AGP

#if 0
void drm_free_agp(struct agp_memory * handle, int pages);
int drm_bind_agp(struct agp_memory * handle, unsigned int start);
int drm_unbind_agp(struct agp_memory * handle);
struct agp_memory *drm_agp_bind_pages(struct drm_device *dev,
				struct page **pages,
				unsigned long num_pages,
				uint32_t gtt_offset,
				uint32_t type);
#endif

int	drm_device_is_agp(struct drm_device *dev);
int	drm_device_is_pcie(struct drm_device *dev);
struct drm_agp_head *drm_agp_init(void);
void drm_agp_clear(struct drm_device *dev);
int drm_agp_acquire(struct drm_device *dev);
int drm_agp_acquire_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);
int drm_agp_release(struct drm_device *dev);
int drm_agp_release_ioctl(struct drm_device *dev, void *data,
			  struct drm_file *file_priv);
int drm_agp_enable(struct drm_device *dev, struct drm_agp_mode mode);
int drm_agp_enable_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv);
void	*drm_agp_allocate_memory(size_t pages, u32 type);
int drm_agp_info(struct drm_device *dev, struct drm_agp_info *info);
int drm_agp_info_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv);
int	drm_agp_free_memory(void *handle);
int	drm_agp_bind_memory(void *handle, off_t start);
int	drm_agp_unbind_memory(void *handle);
int drm_agp_alloc(struct drm_device *dev, struct drm_agp_buffer *request);
int drm_agp_alloc_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file_priv);
int drm_agp_free(struct drm_device *dev, struct drm_agp_buffer *request);
int drm_agp_free_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv);
int drm_agp_unbind(struct drm_device *dev, struct drm_agp_binding *request);
int drm_agp_unbind_ioctl(struct drm_device *dev, void *data,
			 struct drm_file *file_priv);
int drm_agp_bind(struct drm_device *dev, struct drm_agp_binding *request);
int drm_agp_bind_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file_priv);
#else /* __OS_HAS_AGP */

static inline void drm_free_agp(struct agp_memory * handle, int pages)
{
}

static inline int drm_bind_agp(struct agp_memory * handle, unsigned int start)
{
	return -ENODEV;
}

static inline int drm_unbind_agp(struct agp_memory * handle)
{
	return -ENODEV;
}

static inline struct agp_memory *drm_agp_bind_pages(struct drm_device *dev,
					      struct page **pages,
					      unsigned long num_pages,
					      uint32_t gtt_offset,
					      uint32_t type)
{
	return NULL;
}

static inline struct drm_agp_head *drm_agp_init(struct drm_device *dev)
{
	return NULL;
}

static inline void drm_agp_clear(struct drm_device *dev)
{
}

static inline int drm_agp_acquire(struct drm_device *dev)
{
	return -ENODEV;
}

static inline int drm_agp_acquire_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_release(struct drm_device *dev)
{
	return -ENODEV;
}

static inline int drm_agp_release_ioctl(struct drm_device *dev, void *data,
					struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_enable(struct drm_device *dev,
				 struct drm_agp_mode mode)
{
	return -ENODEV;
}

static inline int drm_agp_enable_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_info(struct drm_device *dev,
			       struct drm_agp_info *info)
{
	return -ENODEV;
}

static inline int drm_agp_info_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_alloc(struct drm_device *dev,
				struct drm_agp_buffer *request)
{
	return -ENODEV;
}

static inline int drm_agp_alloc_ioctl(struct drm_device *dev, void *data,
				      struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_free(struct drm_device *dev,
			       struct drm_agp_buffer *request)
{
	return -ENODEV;
}

static inline int drm_agp_free_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_unbind(struct drm_device *dev,
				 struct drm_agp_binding *request)
{
	return -ENODEV;
}

static inline int drm_agp_unbind_ioctl(struct drm_device *dev, void *data,
				       struct drm_file *file_priv)
{
	return -ENODEV;
}

static inline int drm_agp_bind(struct drm_device *dev,
			       struct drm_agp_binding *request)
{
	return -ENODEV;
}

static inline int drm_agp_bind_ioctl(struct drm_device *dev, void *data,
				     struct drm_file *file_priv)
{
	return -ENODEV;
}
#endif /* __OS_HAS_AGP */

#endif /* _DRM_AGPSUPPORT_H_ */
