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

#include "zomlang/compiler/source/manager.h"

#include "zc/core/common.h"
#include "zc/core/string.h"
#include "zc/ztest/test.h"
#include "zomlang/compiler/source/module.h"

namespace zomlang {
namespace compiler {

class TestClock final : public zc::Clock {
public:
  void tick() { time += 1 * zc::SECONDS; }

  [[nodiscard]] zc::Date now() const override { return time; }

  void expectChanged(const zc::FsNode& file) {
    ZC_EXPECT(file.stat().lastModified == time);
    time += 1 * zc::SECONDS;
  }
  void expectUnchanged(const zc::FsNode& file) { ZC_EXPECT(file.stat().lastModified != time); }

private:
  zc::Date time = zc::UNIX_EPOCH + 1 * zc::SECONDS;
};

ZC_TEST("ModuleLoader basic") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);
  clock.expectChanged(*dir);

  zc::Own<const zc::Directory> dir1 = dir->openSubdir(zc::Path("dir1"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir);
  dir1->openFile(zc::Path("mod.zom"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir1);

  zc::Own<const zc::Directory> dir2 = dir->openSubdir(zc::Path("dir2"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir);
  dir2->openFile(zc::Path("mod.zom"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir2);

  source::ModuleLoader loader;

  // Testing for normal loading
  auto subdir1 = dir->openSubdir(zc::Path("dir1"));
  clock.expectUnchanged(*dir);
  clock.expectUnchanged(*subdir1);
  auto mod1 = loader.loadModule(*subdir1, zc::Path("mod.zom"));
  ZC_EXPECT(mod1 != zc::none);

  // Testing reloading
  auto mod2 = loader.loadModule(*subdir1, zc::Path("mod.zom"));
  ZC_EXPECT(mod2 != zc::none);

  auto maybeModule1 = loader.loadModule(*subdir1, zc::Path("mod.zom"));
  auto maybeModule2 = loader.loadModule(*subdir1, zc::Path("mod.zom"));
  ZC_EXPECT(maybeModule1 != zc::none);
  ZC_EXPECT(maybeModule2 != zc::none);

  ZC_IF_SOME(module1, maybeModule1) {
    ZC_IF_SOME(module2, maybeModule2) { ZC_EXPECT(module1 == module2); }
    // Test files with the same name in different directories
    auto subdir2 = dir->openSubdir(zc::Path("dir2"));
    clock.expectUnchanged(*dir);
    auto maybeModule3 = loader.loadModule(*subdir2, zc::Path("mod.zom"));
    ZC_EXPECT(maybeModule3 != zc::none);
    ZC_IF_SOME(module3, maybeModule3) { ZC_EXPECT(module1 != module3); }
  }
}

ZC_TEST("ModuleLoader LoadDuplicateFiles") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);
  clock.expectChanged(*dir);

  auto dir1 = dir->openSubdir(zc::Path("dir1"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir);
  auto file1 = dir1->openFile(zc::Path("test.zom"), zc::WriteMode::CREATE);

  auto dir2 = dir->openSubdir(zc::Path("dir2"), zc::WriteMode::CREATE);
  clock.expectChanged(*dir);
  auto file2 = dir2->openFile(zc::Path("test.zom"), zc::WriteMode::CREATE);

  source::ModuleLoader loader;
  auto readableDir1 = dir->openSubdir(zc::Path("dir1"));
  clock.expectUnchanged(*dir);
  clock.expectUnchanged(*readableDir1);

  auto maybeModule1 = loader.loadModule(*readableDir1, zc::Path("test.zom"));
  auto maybeModule2 = loader.loadModule(*readableDir1, zc::Path("test.zom"));
  ZC_EXPECT(maybeModule1 != zc::none);
  ZC_EXPECT(maybeModule2 != zc::none);

  ZC_IF_SOME(module1, maybeModule1) {
    ZC_IF_SOME(module2, maybeModule2) { ZC_EXPECT(module1 == module2); }
  }

  // Test loading file with same name from different directory
  auto readableDir2 = dir->openSubdir(zc::Path("dir2"));
  clock.expectUnchanged(*dir);
  auto maybeModule3 = loader.loadModule(*readableDir2, zc::Path("test.zom"));
  ZC_EXPECT(maybeModule3 != zc::none);
  ZC_IF_SOME(module3, maybeModule3) {
    ZC_IF_SOME(module1, maybeModule1) { ZC_EXPECT(module1 != module3); }
  }
}

ZC_TEST("ModuleLoader TestModuleIdsUnique") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);
  clock.expectChanged(*dir);

  auto subdir1 = dir->openSubdir(zc::Path("dir1"), zc::WriteMode::CREATE);
  auto file1 = subdir1->openFile(zc::Path("mod1.zom"), zc::WriteMode::CREATE);
  auto subdir2 = dir->openSubdir(zc::Path("dir2"), zc::WriteMode::CREATE);
  auto file2 = subdir2->openFile(zc::Path("mod2.zom"), zc::WriteMode::CREATE);

  source::ModuleLoader loader;

  // Load two different modules
  auto maybeMod1 = loader.loadModule(*subdir1, zc::Path("mod1.zom"));
  auto maybeMod2 = loader.loadModule(*subdir2, zc::Path("mod2.zom"));

  ZC_EXPECT(maybeMod1 != zc::none);
  ZC_EXPECT(maybeMod2 != zc::none);

  ZC_IF_SOME(mod1, maybeMod1) {
    ZC_IF_SOME(mod2, maybeMod2) {
      ZC_EXPECT(mod1.getModuleId() != mod2.getModuleId());  // IDs should differ
    }
  }

  // Reload same module
  auto maybeMod1Reload = loader.loadModule(*subdir1, zc::Path("mod1.zom"));
  ZC_IF_SOME(mod1, maybeMod1) {
    ZC_IF_SOME(mod1Reload, maybeMod1Reload) {
      ZC_EXPECT(mod1.getModuleId() == mod1Reload.getModuleId());  // Same ID
    }
  }
}

ZC_TEST("ModuleLoader TestFileContentChange") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);
  auto subdir = dir->openSubdir(zc::Path("src"), zc::WriteMode::CREATE);

  // Create an initial file
  {
    auto file = subdir->openFile(zc::Path("test.zom"), zc::WriteMode::CREATE);
    file->writeAll("content v1");
    clock.expectChanged(*subdir);
  }

  source::ModuleLoader loader;
  auto maybeMod1 = loader.loadModule(*subdir, zc::Path("test.zom"));
  ZC_EXPECT(maybeMod1 != zc::none);

  // Modify file content
  {
    auto file = subdir->openFile(zc::Path("test.zom"), zc::WriteMode::MODIFY);
    file->writeAll("content v2");  // Different content
    clock.expectChanged(*file);
  }

  auto maybeMod2 = loader.loadModule(*subdir, zc::Path("test.zom"));
  ZC_EXPECT(maybeMod2 != zc::none);

  // Verify new module created
  ZC_IF_SOME(mod1, maybeMod1) {
    ZC_IF_SOME(mod2, maybeMod2) {
      ZC_EXPECT(mod1 != mod2);  // Should be different modules
    }
  }
}

ZC_TEST("ModuleLoader TestInvalidPath") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);
  source::ModuleLoader loader;

  // Try loading a non-existent file
  auto subdir = dir->openSubdir(zc::Path("src"), zc::WriteMode::CREATE);
  auto result = loader.loadModule(*subdir, zc::Path("ghost.zom"));
  ZC_EXPECT(result == zc::none);  // Should fail to load
}

ZC_TEST("ModuleLoader TestSameContentDifferentPaths") {
  TestClock clock;

  auto dir = newInMemoryDirectory(clock);

  // Create two files with the same content
  auto subdir1 = dir->openSubdir(zc::Path("dir1"), zc::WriteMode::CREATE);
  auto file1 = subdir1->openFile(zc::Path("file.zom"), zc::WriteMode::CREATE);
  file1->writeAll("same content");

  auto subdir2 = dir->openSubdir(zc::Path("dir2"), zc::WriteMode::CREATE);
  auto file2 = subdir2->openFile(zc::Path("file.zom"), zc::WriteMode::CREATE);
  file2->writeAll("same content");

  source::ModuleLoader loader;

  auto maybeMod1 = loader.loadModule(*subdir1, zc::Path("file.zom"));
  auto maybeMod2 = loader.loadModule(*subdir2, zc::Path("file.zom"));

  ZC_EXPECT(maybeMod1 != zc::none);
  ZC_EXPECT(maybeMod2 != zc::none);

  // Should be considered different modules despite the same content
  ZC_IF_SOME(mod1, maybeMod1) {
    ZC_IF_SOME(mod2, maybeMod2) { ZC_EXPECT(mod1 != mod2); }
  }
}

}  // namespace compiler
}  // namespace zomlang
