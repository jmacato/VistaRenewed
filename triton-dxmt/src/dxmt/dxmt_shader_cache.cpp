#include "dxmt_shader_cache.hpp"
#include "airconv_metal_target.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

namespace dxmt {

/* The cache stores airconv output, so its identity is the converter version
 * plus the AIR target it emits (platform triple, OS floor).  Fold both into
 * the version: a stale entry built for another target must never be replayed,
 * or PSO creation fails at the AGX backend ("Failed to materializeAll" /
 * "Target OS is incompatible") for as long as the entry lives. */
static uint64_t
cacheVersion(WMTMetalVersion metal_version) {
  auto target = GetMetalTarget((SM50_SHADER_METAL_VERSION)metal_version);
  uint64_t h = 0xcbf29ce484222325ull;
  for (char c : target.triple)
    h = (h ^ (uint8_t)c) * 0x100000001b3ull;
  return h ^ (uint64_t)kDXMTShaderCacheVersion;
}

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<dxmt::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  uint64_t version = cacheVersion(metal_version);
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), version);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), version);
}

} // namespace dxmt