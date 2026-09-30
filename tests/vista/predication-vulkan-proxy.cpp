/* SPDX-License-Identifier: MIT
 * Test-only Vulkan forwarding proxy for predication observation/fault injection. */
#include <vulkan/vulkan.h>
#include <algorithm>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <atomic>
static PFN_vkGetInstanceProcAddr realInstance;
static PFN_vkGetDeviceProcAddr realDevice;
static PFN_vkEnumerateDeviceExtensionProperties realEnumerate;
static PFN_vkCmdBeginConditionalRenderingEXT realBegin;
static PFN_vkCmdEndConditionalRenderingEXT realEnd;
static PFN_vkCmdDrawIndirectByteCountEXT realByteCount;
static std::atomic<unsigned> blocks{0};
static std::atomic<unsigned> byteCountCalls{0}, invalidByteCountCalls{0};
static VkResult VKAPI_CALL enumerate(VkPhysicalDevice device,const char* layer,uint32_t* count,VkExtensionProperties* out) {
  if(!getenv("PREDICATION_VK_HIDE"))return realEnumerate(device,layer,count,out);
  uint32_t n=0;VkResult r=realEnumerate(device,layer,&n,nullptr);if(r!=VK_SUCCESS)return r;
  std::vector<VkExtensionProperties> all(n);r=realEnumerate(device,layer,&n,all.data());if(r!=VK_SUCCESS)return r;
  std::vector<VkExtensionProperties> filtered;for(auto& p:all)if(strcmp(p.extensionName,VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME))filtered.push_back(p);
  if(!out){*count=filtered.size();return VK_SUCCESS;}uint32_t copied=std::min(*count,uint32_t(filtered.size()));
  memcpy(out,filtered.data(),copied*sizeof(*out));*count=copied;return copied<filtered.size()?VK_INCOMPLETE:VK_SUCCESS;
}
static void VKAPI_CALL begin(VkCommandBuffer cmd,const VkConditionalRenderingBeginInfoEXT* info){blocks++;if(!getenv("PREDICATION_VK_DROP"))realBegin(cmd,info);}
static void VKAPI_CALL end(VkCommandBuffer cmd){if(!getenv("PREDICATION_VK_DROP"))realEnd(cmd);}
static void VKAPI_CALL byteCount(VkCommandBuffer cmd,uint32_t instances,uint32_t first,VkBuffer buffer,VkDeviceSize offset,uint32_t bias,uint32_t stride) {
  byteCountCalls++;
  const bool valid=!(offset&3u)&&!(bias&3u)&&stride&&!(stride&3u);
  fprintf(stderr,"[DRAWAUTO-VULKAN] counter_buffer_offset=%llu bias=%u stride=%u valid=%u\n",static_cast<unsigned long long>(offset),bias,stride,valid);
  if(!valid){invalidByteCountCalls++;std::abort();}
  realByteCount(cmd,instances,first,buffer,offset,bias,stride);
}
static PFN_vkVoidFunction VKAPI_CALL deviceProc(VkDevice dev,const char* name){
  auto proc=realDevice(dev,name);
  if(!strcmp(name,"vkCmdBeginConditionalRenderingEXT")){realBegin=reinterpret_cast<PFN_vkCmdBeginConditionalRenderingEXT>(proc);return reinterpret_cast<PFN_vkVoidFunction>(begin);}
  if(!strcmp(name,"vkCmdEndConditionalRenderingEXT")){realEnd=reinterpret_cast<PFN_vkCmdEndConditionalRenderingEXT>(proc);return reinterpret_cast<PFN_vkVoidFunction>(end);}
  if(proc&&!strcmp(name,"vkCmdDrawIndirectByteCountEXT")){realByteCount=reinterpret_cast<PFN_vkCmdDrawIndirectByteCountEXT>(proc);return reinterpret_cast<PFN_vkVoidFunction>(byteCount);}
  return proc;
}
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance,const char* name){
  if(!realInstance){void* lib=dlopen("libvulkan.so.1",RTLD_NOW|RTLD_LOCAL);if(!lib)std::abort();realInstance=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib,"vkGetInstanceProcAddr"));}
  auto proc=realInstance(instance,name);
  if(!strcmp(name,"vkEnumerateDeviceExtensionProperties")){realEnumerate=reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(proc);return reinterpret_cast<PFN_vkVoidFunction>(enumerate);}
  if(!strcmp(name,"vkGetDeviceProcAddr")){realDevice=reinterpret_cast<PFN_vkGetDeviceProcAddr>(proc);return reinterpret_cast<PFN_vkVoidFunction>(deviceProc);}
  return proc;
}
__attribute__((destructor)) static void report(){fprintf(stderr,"[PREDICATE-VULKAN] blocks=%u hidden=%u dropped=%u byte_count_calls=%u invalid_byte_count_calls=%u\n",blocks.load(),!!getenv("PREDICATION_VK_HIDE"),!!getenv("PREDICATION_VK_DROP"),byteCountCalls.load(),invalidByteCountCalls.load());}
