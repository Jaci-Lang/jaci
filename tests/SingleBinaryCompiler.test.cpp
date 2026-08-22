// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
// Copyright (c) 2026 Júlia Klee, Roblox Corporation, Lua.org/PUC-Rio. MIT License.
#include "Luau/SingleBinaryCompiler.h"
#include "Luau/FileUtils.h"
#include "doctest.h"

#include <cstdlib>
#include <fstream>
#include <string>

TEST_SUITE_BEGIN("SingleBinaryCompilerTests");

TEST_CASE("SingleBinaryEndToEndCompilation")
{
    std::string testDir = "/tmp/jaci_single_bin_test";
    system(("mkdir -p " + testDir).c_str());

    std::ofstream helper(testDir + "/helper.luau");
    helper << "local H = {}\n";
    helper << "function H.greet(name: string): string\n";
    helper << "    return 'Hello, ' .. name .. '!'\n";
    helper << "end\n";
    helper << "return H\n";
    helper.close();

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local H = require('./helper')\n";
    mainFile << "print(H.greet('Jaci Single Binary'))\n";
    mainFile.close();

    std::string outputBinary = testDir + "/out_app";

    Luau::SingleBinaryOptions options;
    options.entryFilePath = testDir + "/main.luau";
    options.outputBinaryPath = outputBinary;
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.codegen = true;
    options.verbose = false;

    bool ok = Luau::SingleBinaryCompiler::compile(options);
    CHECK(ok);
    CHECK(isFile(outputBinary));

    // Execute generated binary
    int ret = system((outputBinary + " > /dev/null 2>&1").c_str());
    CHECK(ret == 0);

    system(("rm -rf " + testDir).c_str());
}

TEST_CASE("SingleBinaryAssetBundlingAndVirtualFs")
{
    std::string testDir = "/tmp/jaci_single_bin_asset_test";
    system(("mkdir -p " + testDir + "/assets").c_str());

    std::ofstream assetFile(testDir + "/assets/data.txt");
    assetFile << "ASSET_PAYLOAD_12345";
    assetFile.close();

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local fs = require('@std/fs')\n";
    mainFile << "assert(fs.exists('./assets/data.txt'))\n";
    mainFile << "assert(fs.readFile('./assets/data.txt') == 'ASSET_PAYLOAD_12345')\n";
    mainFile.close();

    std::string outputBinary = testDir + "/asset_app";

    Luau::SingleBinaryOptions options;
    options.entryFilePath = testDir + "/main.luau";
    options.outputBinaryPath = outputBinary;
    options.assetPaths = { testDir + "/assets" };
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.codegen = true;
    options.verbose = false;

    bool ok = Luau::SingleBinaryCompiler::compile(options);
    CHECK(ok);
    CHECK(isFile(outputBinary));

    // Execute generated binary in /tmp to ensure zero disk dependencies on relative assets
    int ret = system(("cd /tmp && " + outputBinary + " > /dev/null 2>&1").c_str());
    CHECK(ret == 0);

    system(("rm -rf " + testDir).c_str());
}

TEST_CASE("SingleBinaryStandardLibraryVirtualImports")
{
    std::string testDir = "/tmp/jaci_single_bin_std_test";
    system(("mkdir -p " + testDir).c_str());

    std::ofstream mainFile(testDir + "/main.luau");
    mainFile << "local fs = require('@std/fs')\n";
    mainFile << "local net = require('@std/net')\n";
    mainFile << "local task = require('@std/task')\n";
    mainFile << "local json = require('@std/json')\n";
    mainFile << "assert(type(fs.readFile) == 'function')\n";
    mainFile << "assert(type(net.request) == 'function')\n";
    mainFile << "assert(type(task.spawn) == 'function')\n";
    mainFile << "assert(type(json.encode) == 'function')\n";
    mainFile.close();

    std::string outputBinary = testDir + "/std_app";

    Luau::SingleBinaryOptions options;
    options.entryFilePath = testDir + "/main.luau";
    options.outputBinaryPath = outputBinary;
    options.targetArchitecture = "linux-x64";
    options.optimizationLevel = 1;
    options.debugLevel = 1;
    options.codegen = true;
    options.verbose = false;

    bool ok = Luau::SingleBinaryCompiler::compile(options);
    CHECK(ok);
    CHECK(isFile(outputBinary));

    int ret = system((outputBinary + " > /dev/null 2>&1").c_str());
    CHECK(ret == 0);

    system(("rm -rf " + testDir).c_str());
}

TEST_SUITE_END();
