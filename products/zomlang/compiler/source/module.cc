// Copyright (c) 2025 Zode.Z. All rights reserved
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations under
// the License.

#include "zomlang/compiler/source/module.h"

#include <unordered_map>

#include "zc/core/debug.h"
#include "zc/core/filesystem.h"
#include "zomlang/compiler/source/manager.h"

namespace zomlang {
namespace compiler {
namespace source {

namespace {

struct FileKey {
  const zc::ReadableDirectory& baseDir;
  zc::PathPtr path;
  zc::Maybe<const zc::ReadableFile&> file;
  uint64_t hashCode;
  uint64_t size;
  zc::Date lastModified;

  FileKey(const zc::ReadableDirectory& baseDir, const zc::PathPtr path)
      : baseDir(baseDir),
        path(path),
        file(zc::none),
        hashCode(0),
        size(0),
        lastModified(zc::UNIX_EPOCH) {}

  FileKey(const zc::ReadableDirectory& baseDir, const zc::PathPtr path,
          const zc::ReadableFile& file)
      : FileKey(baseDir, path, file, file.stat()) {}

  FileKey(const zc::ReadableDirectory& baseDir, const zc::PathPtr path,
          const zc::ReadableFile& file, const zc::FsNode::Metadata& meta)
      : baseDir(baseDir),
        path(path),
        file(file),
        hashCode(meta.hashCode),
        size(meta.size),
        lastModified(meta.lastModified) {}

  bool operator==(const FileKey& other) const {
    if (&baseDir == &other.baseDir && path == other.path) return true;

    if (hashCode != other.hashCode || size != other.size || lastModified != other.lastModified)
      return false;

    if (path.size() > 0 && other.path.size() > 0 &&
        path[path.size() - 1] != other.path[other.path.size() - 1])
      return false;

    const auto mapping1 = ZC_ASSERT_NONNULL(file).mmap(0, size);
    const auto mapping2 = ZC_ASSERT_NONNULL(other.file).mmap(0, size);
    return mapping1 == mapping2;
  }
};

struct FileKeyHash {
  size_t operator()(const FileKey& key) const {
    constexpr size_t prime = 0x9e3779b97f4a7c15;
    size_t seed = key.hashCode;

    for (auto& part : key.path) {
      seed ^= zc::hashCode<zc::StringPtr>(part) + prime + (seed << 6) + (seed >> 2);
    }

    seed = (seed ^ key.size * prime) * prime;
    seed = (seed ^ (key.lastModified - zc::UNIX_EPOCH) / zc::MILLISECONDS * prime) * prime;

    if constexpr (sizeof(size_t) < sizeof(decltype(key.hashCode))) { return (seed >> 32) ^ seed; }
    return seed;
  }
};

}  // namespace

// ================================================================================
// Module::Impl

class Module::Impl {
public:
  Impl(zc::StringPtr moduleName, uint64_t id) noexcept;
  ~Impl() noexcept(false) = default;

  ZC_DISALLOW_COPY_AND_MOVE(Impl);

  /// Returns the source name of this module.
  zc::StringPtr getModuleName();
  /// Returns true if this module is compiled.
  ZC_NODISCARD bool isCompiled() const;
  /// Retrieves the unique ID of the module
  ZC_NODISCARD uint64_t getModuleId() const;
  void markCompiled();

private:
  zc::String moduleName;
  const uint64_t moduleId;

  bool compiled;
};

Module::Impl::Impl(zc::StringPtr moduleName, const uint64_t id) noexcept
    : moduleName(zc::str(moduleName)), moduleId(id), compiled(false) {
  ZC_REQUIRE(moduleName.size() > 0);
}

zc::StringPtr Module::Impl::getModuleName() { return moduleName; }
bool Module::Impl::isCompiled() const { return compiled; }
uint64_t Module::Impl::getModuleId() const { return moduleId; }
void Module::Impl::markCompiled() { compiled = true; }

// ================================================================================
// Module

Module::Module(zc::StringPtr moduleName, uint64_t id) noexcept
    : impl(zc::heap<Impl>(moduleName, id)) {};
Module::~Module() noexcept(false) = default;

// static
zc::Own<Module> Module::create(zc::StringPtr moduleName, uint64_t id) {
  return zc::heap<Module>(moduleName, id);
}
zc::StringPtr Module::getModuleName() { return impl->getModuleName(); }
bool Module::isCompiled() const { return impl->isCompiled(); }
uint64_t Module::getModuleId() const { return impl->getModuleId(); }
void Module::markCompiled() { impl->markCompiled(); }

// ================================================================================
// ModuleLoader

ModuleLoader::ModuleLoader() : impl(zc::heap<Impl>()) {}
ModuleLoader::~ModuleLoader() noexcept(false) = default;

// ================================================================================
// ModuleLoader::Impl

class ModuleLoader::Impl {
public:
  Impl() noexcept : nextModuleId(0) {}
  ~Impl() noexcept(false) = default;

  zc::Maybe<const Module&> loadModule(const zc::ReadableDirectory& dir, zc::PathPtr path);
  zc::Maybe<const Module&> loadModule(const zc::StringPtr moduleName, const uint64_t moduleId);

private:
  std::unordered_map<FileKey, zc::Own<Module>, FileKeyHash> modules;
  uint64_t nextModuleId;
};

zc::Maybe<const Module&> ModuleLoader::loadModule(const zc::ReadableDirectory& dir,
                                                  const zc::PathPtr path) {
  return impl->loadModule(dir, path);
}

zc::Maybe<const Module&> ModuleLoader::loadModule(const zc::StringPtr moduleName,
                                                  const uint64_t moduleId) {
  return impl->loadModule(moduleName, moduleId);
}

zc::Maybe<const Module&> ModuleLoader::Impl::loadModule(const zc::ReadableDirectory& dir,
                                                        zc::PathPtr path) {
  ZC_IF_SOME(file, dir.tryOpenFile(path)) {
    const zc::Path pathCopy = path.clone();
    auto key = FileKey(dir, pathCopy, *file);
    if (const auto it = modules.find(key); it != modules.end()) { return *it->second; }

    const uint64_t id = nextModuleId++;
    zc::Own<Module> module = Module::create(path.toString(), id);

    auto& result = *module;
    const auto [fst, snd] = modules.insert(std::make_pair(zc::mv(key), zc::mv(module)));
    if (snd) { return result; }
    // Now that we have the file open, we noticed a collision. Return the old file.
    return *fst->second;
  }

  return zc::none;
}

zc::Maybe<const Module&> ModuleLoader::Impl::loadModule(const zc::StringPtr moduleName,
                                                        const uint64_t moduleId) {
  zc::Own<Module> module = Module::create(moduleName, moduleId);
  return zc::none;

  // auto& result = *module;
  // const auto [fst, snd] = modules.insert(std::make_pair(zc::mv(key), zc::mv(module)));
  // if (snd) { return result; }
  // // Now that we have the file open, we noticed a collision. Return the old file.
  // return *fst->second;
}

}  // namespace source
}  // namespace compiler
}  // namespace zomlang
