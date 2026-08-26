// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/Common.h"
#include "Luau/FileUtils.h"
#include "Luau/Repl.h"
#include "Luau/ReplRequirer.h"
#include "Luau/Require.h"
#include "Luau/VfsNavigator.h"
#include "lua.h"
#include "lualib.h"
#include "doctest.h"

#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

TEST_SUITE_BEGIN("UniversalRequireTests");

TEST_CASE("VfsNavigatorIndexLuauSupport")
{
    std::string testDir = "/tmp/jaci_test_index_dir";
    system(("mkdir -p " + testDir + "/sub_module").c_str());

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local sub = require('./sub_module')\n";
    mainFile.close();

    std::ofstream out(testDir + "/sub_module/index.luau");
    out << "return { status = 'ok_from_index' }\n";
    out.close();

    VfsNavigator nav;
    NavigationStatus status = nav.resetToPath(testDir + "/main.luau");
    CHECK(status == NavigationStatus::Success);

    status = nav.toParent();
    CHECK(status == NavigationStatus::Success);

    status = nav.toChild("sub_module");
    CHECK(status == NavigationStatus::Success);
    CHECK(nav.getFilePath().find("index.luau") != std::string::npos);

    system(("rm -rf " + testDir).c_str());
}

TEST_CASE("VfsNavigatorBarePackageResolution")
{
    std::string testDir = "/tmp/jaci_test_bare_pkg";
    system(("mkdir -p " + testDir + "/packages/awesome_lib").c_str());
    system(("mkdir -p " + testDir + "/src").c_str());

    std::ofstream mainFile(testDir + "/src/main.luau");
    mainFile << "local lib = require('awesome_lib')\nreturn lib.version\n";
    mainFile.close();

    std::ofstream out(testDir + "/packages/awesome_lib/init.luau");
    out << "return { version = '1.0.0' }\n";
    out.close();

    VfsNavigator nav;
    NavigationStatus status = nav.resetToPath(testDir + "/src/main.luau");
    CHECK(status == NavigationStatus::Success);

    status = nav.toBarePackage("awesome_lib");
    CHECK(status == NavigationStatus::Success);
    CHECK(nav.getFilePath().find("packages/awesome_lib") != std::string::npos);

    system(("rm -rf " + testDir).c_str());
}

TEST_CASE("UniversalRequireEndToEndExecution")
{
    std::string testDir = "/tmp/jaci_e2e_require";
    system(("mkdir -p " + testDir + "/packages/calc").c_str());

    std::ofstream calcFile(testDir + "/packages/calc/init.luau");
    calcFile << "return { multiply = function(a: number, b: number) return a * b end }\n";
    calcFile.close();

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local calc = require('./packages/calc')\nreturn calc.multiply(6, 7)\n";
    mainFile.close();

    std::string runner = "./build/luau " + testDir + "/main.luau";
    int ret = system(runner.c_str());
    CHECK(ret == 0);

    system(("rm -rf " + testDir).c_str());
}

TEST_CASE("UniversalRequirePropagatesCompileErrors")
{
    std::string testDir = "/tmp/jaci_e2e_require_compile_error";
    system(("mkdir -p " + testDir).c_str());

    std::ofstream badModule(testDir + "/bad.luau");
    badModule << "local = broken\n";
    badModule.close();

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local ok, err = pcall(require, './bad')\n"
                "assert(not ok)\n"
                "assert(type(err) == 'string' and string.find(err, 'compiling module', 1, true))\n";
    mainFile.close();

    std::string runner = "./build/luau " + testDir + "/main.luau";
    int ret = system(runner.c_str());
    CHECK(ret == 0);

    system(("rm -rf " + testDir).c_str());
}

TEST_SUITE_END();
