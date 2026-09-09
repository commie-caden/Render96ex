/*
 * Single translation unit that instantiates VMA. Kept apart so the 20k-line
 * header is compiled exactly once and its warnings stay out of our sources.
 */
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
