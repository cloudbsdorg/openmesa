/*
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: MIT
 *
 * CloudBSD private fork (FORK-NOTES.md).
 *
 * nvk-nvrm-probe: for every Vulkan physical device, print what
 * "vulkaninfo --summary" prints for it (API and driver versions, type,
 * driver id, name and info, vendor and device id, queue families, memory
 * heaps), then try vkCreateDevice with one queue of the first family and
 * print the exact VkResult.  vulkaninfo itself creates a device before it
 * prints anything, so it cannot run on the stub; this probe needs none.  Output is one JSON object per device on stdout (JSON
 * Lines), with the stub message the backend is expected to log, taken from
 * the same header the backend uses.  Exit 0 when every device was probed,
 * 1 when the instance could not be created, 2 when no device was found.
 *
 * Used by "make test" (fake libnvrm backend) and by the test-boot row
 * nvk-nvrm-device (real GPU).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "nvkmd/nvrm/nvkmd_nvrm_msg.h"

static const char *
result_name(VkResult r)
{
	switch (r) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	default: return "VK_RESULT_OTHER";
	}
}

/* Prints s as a JSON string (the names here are plain ASCII). */
static void
json_str(const char *s)
{
	putchar('"');
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			printf("\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			printf("\\u%04x", (unsigned char)*s);
		else
			putchar(*s);
	}
	putchar('"');
}

static void
probe_device(VkPhysicalDevice pd, uint32_t index)
{
	VkPhysicalDeviceDriverProperties drv = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 props = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = &drv,
	};
	vkGetPhysicalDeviceProperties2(pd, &props);

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = 0,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
	};
	/* The stub's line goes to stderr just before the result. */
	fflush(stdout);
	VkDevice dev = VK_NULL_HANDLE;
	VkResult r = vkCreateDevice(pd, &dci, NULL, &dev);
	if (r == VK_SUCCESS)
		vkDestroyDevice(dev, NULL);

	printf("{\"schema\":\"nvk-nvrm-probe/1\",\"index\":%u,"
	    "\"driverID\":%d,\"driverIsMesaNvk\":%s,\"vendorID\":%u,"
	    "\"deviceID\":%u,\"deviceName\":", index, (int)drv.driverID,
	    drv.driverID == VK_DRIVER_ID_MESA_NVK ? "true" : "false",
	    props.properties.vendorID, props.properties.deviceID);
	json_str(props.properties.deviceName);
	printf(",\"apiVersion\":\"%u.%u.%u\",\"driverVersion\":%u,"
	    "\"deviceType\":%d,\"driverName\":",
	    VK_API_VERSION_MAJOR(props.properties.apiVersion),
	    VK_API_VERSION_MINOR(props.properties.apiVersion),
	    VK_API_VERSION_PATCH(props.properties.apiVersion),
	    props.properties.driverVersion,
	    (int)props.properties.deviceType);
	json_str(drv.driverName);
	printf(",\"driverInfo\":");
	json_str(drv.driverInfo);

	uint32_t nq = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
	VkQueueFamilyProperties qf[16];
	if (nq > 16)
		nq = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
	printf(",\"queueFamilies\":[");
	for (uint32_t q = 0; q < nq; q++)
		printf("%s{\"flags\":%u,\"count\":%u}", q ? "," : "",
		    qf[q].queueFlags, qf[q].queueCount);
	printf("]");

	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	printf(",\"memoryHeaps\":[");
	for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
		printf("%s{\"size\":%llu,\"flags\":%u}", h ? "," : "",
		    (unsigned long long)mp.memoryHeaps[h].size,
		    mp.memoryHeaps[h].flags);
	printf("],\"memoryTypes\":[");
	for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
		printf("%s{\"heap\":%u,\"flags\":%u}", t ? "," : "",
		    mp.memoryTypes[t].heapIndex,
		    mp.memoryTypes[t].propertyFlags);
	printf("]");

	printf(",\"createDevice\":\"%s\",\"stubMessage\":",
	    result_name(r));
	json_str(NVK_NVRM_STUB_MSG);
	printf(",\"expectedDriverName\":");
	json_str(NVK_NVRM_DRIVER_NAME);
	printf("}\n");
}

int
main(void)
{
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "nvk-nvrm-probe",
		.apiVersion = VK_API_VERSION_1_3,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance inst;
	VkResult r = vkCreateInstance(&ici, NULL, &inst);
	if (r != VK_SUCCESS) {
		printf("{\"schema\":\"nvk-nvrm-probe/1\",\"error\":\"vkCreateInstance\","
		    "\"result\":\"%s\"}\n", result_name(r));
		return (1);
	}

	uint32_t n = 0;
	r = vkEnumeratePhysicalDevices(inst, &n, NULL);
	if (r != VK_SUCCESS || n == 0) {
		printf("{\"schema\":\"nvk-nvrm-probe/1\",\"error\":\"no physical device\","
		    "\"result\":\"%s\"}\n", result_name(r));
		vkDestroyInstance(inst, NULL);
		return (2);
	}
	VkPhysicalDevice *pds = calloc(n, sizeof(*pds));
	if (pds == NULL) {
		vkDestroyInstance(inst, NULL);
		return (1);
	}
	vkEnumeratePhysicalDevices(inst, &n, pds);

	for (uint32_t i = 0; i < n; i++)
		probe_device(pds[i], i);

	free(pds);
	vkDestroyInstance(inst, NULL);
	return (0);
}
