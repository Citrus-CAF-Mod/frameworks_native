/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <fcntl.h>
#include <libgen.h>

#include <android-base/file.h>
#include <cutils/properties.h>
#include <ziparchive/zip_archive.h>

#include "dumpstate.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

namespace android {
namespace os {
namespace dumpstate {

using ::testing::Test;
using ::std::literals::chrono_literals::operator""s;

struct SectionInfo {
    std::string name;
    status_t status;
    int32_t size_bytes;
    int32_t duration_ms;
};

/**
 * Listens to bugreport progress and updates the user by writing the progress to STDOUT. All the
 * section details generated by dumpstate are added to a vector to be used by Tests later.
 */
class DumpstateListener : public IDumpstateListener {
  public:
    int outFd_, max_progress_;
    std::shared_ptr<std::vector<SectionInfo>> sections_;
    DumpstateListener(int fd, std::shared_ptr<std::vector<SectionInfo>> sections)
        : outFd_(fd), max_progress_(5000), sections_(sections) {
    }
    binder::Status onProgressUpdated(int32_t progress) override {
        dprintf(outFd_, "\rIn progress %d/%d", progress, max_progress_);
        return binder::Status::ok();
    }
    binder::Status onMaxProgressUpdated(int32_t max_progress) override {
        max_progress_ = max_progress;
        return binder::Status::ok();
    }
    binder::Status onSectionComplete(const ::std::string& name, int32_t status, int32_t size_bytes,
                                     int32_t duration_ms) override {
        sections_->push_back({name, status, size_bytes, duration_ms});
        return binder::Status::ok();
    }
    IBinder* onAsBinder() override {
        return nullptr;
    }
};

/**
 * Generates bug report and provide access to the bug report file and other info for other tests.
 * Since bug report generation is slow, the bugreport is only generated once.
 */
class ZippedBugreportGenerationTest : public Test {
  public:
    static std::shared_ptr<std::vector<SectionInfo>> sections;
    static Dumpstate& ds;
    static std::chrono::milliseconds duration;
    static void SetUpTestCase() {
        property_set("dumpstate.options", "bugreportplus");
        // clang-format off
        char* argv[] = {
            (char*)"dumpstate",
            (char*)"-d",
            (char*)"-z",
            (char*)"-B",
            (char*)"-o",
            (char*)dirname(android::base::GetExecutablePath().c_str())
        };
        // clang-format on
        sp<DumpstateListener> listener(new DumpstateListener(dup(fileno(stdout)), sections));
        ds.listener_ = listener;
        ds.listener_name_ = "Smokey";
        ds.report_section_ = true;
        auto start = std::chrono::steady_clock::now();
        run_main(ARRAY_SIZE(argv), argv);
        auto end = std::chrono::steady_clock::now();
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    }

    static const char* getZipFilePath() {
        return ds.GetPath(".zip").c_str();
    }
};
std::shared_ptr<std::vector<SectionInfo>> ZippedBugreportGenerationTest::sections =
    std::make_shared<std::vector<SectionInfo>>();
Dumpstate& ZippedBugreportGenerationTest::ds = Dumpstate::GetInstance();
std::chrono::milliseconds ZippedBugreportGenerationTest::duration = 0s;

TEST_F(ZippedBugreportGenerationTest, IsGeneratedWithoutErrors) {
    EXPECT_EQ(access(getZipFilePath(), F_OK), 0);
}

TEST_F(ZippedBugreportGenerationTest, Is3MBto30MBinSize) {
    struct stat st;
    EXPECT_EQ(stat(getZipFilePath(), &st), 0);
    EXPECT_GE(st.st_size, 3000000 /* 3MB */);
    EXPECT_LE(st.st_size, 30000000 /* 30MB */);
}

TEST_F(ZippedBugreportGenerationTest, TakesBetween30And150Seconds) {
    EXPECT_GE(duration, 30s) << "Expected completion in more than 30s. Actual time "
                             << duration.count() << " s.";
    EXPECT_LE(duration, 150s) << "Expected completion in less than 150s. Actual time "
                              << duration.count() << " s.";
}

/**
 * Run tests on contents of zipped bug report.
 */
class ZippedBugReportContentsTest : public Test {
  public:
    ZipArchiveHandle handle;
    void SetUp() {
        ASSERT_EQ(OpenArchive(ZippedBugreportGenerationTest::getZipFilePath(), &handle), 0);
    }
    void TearDown() {
        CloseArchive(handle);
    }

    void FileExists(const char* filename, uint32_t minsize, uint32_t maxsize) {
        ZipEntry entry;
        EXPECT_EQ(FindEntry(handle, ZipString(filename), &entry), 0);
        EXPECT_GT(entry.uncompressed_length, minsize);
        EXPECT_LT(entry.uncompressed_length, maxsize);
    }
};

TEST_F(ZippedBugReportContentsTest, ContainsMainEntry) {
    ZipEntry mainEntryLoc;
    // contains main entry name file
    EXPECT_EQ(FindEntry(handle, ZipString("main_entry.txt"), &mainEntryLoc), 0);

    char* buf = new char[mainEntryLoc.uncompressed_length];
    ExtractToMemory(handle, &mainEntryLoc, (uint8_t*)buf, mainEntryLoc.uncompressed_length);
    delete[] buf;

    // contains main entry file
    FileExists(buf, 1000000U, 50000000U);
}

TEST_F(ZippedBugReportContentsTest, ContainsVersion) {
    ZipEntry entry;
    // contains main entry name file
    EXPECT_EQ(FindEntry(handle, ZipString("version.txt"), &entry), 0);

    char* buf = new char[entry.uncompressed_length + 1];
    ExtractToMemory(handle, &entry, (uint8_t*)buf, entry.uncompressed_length);
    buf[entry.uncompressed_length] = 0;
    EXPECT_STREQ(buf, ZippedBugreportGenerationTest::ds.version_.c_str());
    delete[] buf;
}

TEST_F(ZippedBugReportContentsTest, ContainsBoardSpecificFiles) {
    FileExists("dumpstate_board.bin", 1000000U, 80000000U);
    FileExists("dumpstate_board.txt", 100000U, 1000000U);
}

// Spot check on some files pulled from the file system
TEST_F(ZippedBugReportContentsTest, ContainsSomeFileSystemFiles) {
    // FS/proc/*/mountinfo size > 0
    FileExists("FS/proc/1/mountinfo", 0U, 100000U);

    // FS/data/misc/profiles/cur/0/*/primary.prof size > 0
    FileExists("FS/data/misc/profiles/cur/0/com.android.phone/primary.prof", 0U, 100000U);
}

/**
 * Runs tests on section data generated by dumpstate and captured by DumpstateListener.
 */
class BugreportSectionTest : public Test {
  public:
    int numMatches(const std::string& substring) {
        int matches = 0;
        for (auto const& section : *ZippedBugreportGenerationTest::sections) {
            if (section.name.find(substring) != std::string::npos) {
                matches++;
            }
        }
        return matches;
    }
    void SectionExists(const std::string& sectionName, int minsize) {
        for (auto const& section : *ZippedBugreportGenerationTest::sections) {
            if (sectionName == section.name) {
                EXPECT_GE(section.size_bytes, minsize);
                return;
            }
        }
        FAIL() << sectionName << " not found.";
    }
};

// Test all sections are generated without timeouts or errors
TEST_F(BugreportSectionTest, GeneratedWithoutErrors) {
    for (auto const& section : *ZippedBugreportGenerationTest::sections) {
        EXPECT_EQ(section.status, 0) << section.name << " failed with status " << section.status;
    }
}

TEST_F(BugreportSectionTest, Atleast3CriticalDumpsysSectionsGenerated) {
    int numSections = numMatches("DUMPSYS CRITICAL");
    EXPECT_GE(numSections, 3);
}

TEST_F(BugreportSectionTest, Atleast2HighDumpsysSectionsGenerated) {
    int numSections = numMatches("DUMPSYS HIGH");
    EXPECT_GE(numSections, 2);
}

TEST_F(BugreportSectionTest, Atleast50NormalDumpsysSectionsGenerated) {
    int allSections = numMatches("DUMPSYS");
    int criticalSections = numMatches("DUMPSYS CRITICAL");
    int highSections = numMatches("DUMPSYS HIGH");
    int normalSections = allSections - criticalSections - highSections;

    EXPECT_GE(normalSections, 50) << "Total sections less than 50 (Critical:" << criticalSections
                                  << "High:" << highSections << "Normal:" << normalSections << ")";
}

TEST_F(BugreportSectionTest, Atleast1ProtoDumpsysSectionGenerated) {
    int numSections = numMatches("proto/");
    EXPECT_GE(numSections, 1);
}

// Test if some critical sections are being generated.
TEST_F(BugreportSectionTest, CriticalSurfaceFlingerSectionGenerated) {
    SectionExists("DUMPSYS CRITICAL - SurfaceFlinger", /* bytes= */ 10000);
}

TEST_F(BugreportSectionTest, ActivitySectionsGenerated) {
    SectionExists("DUMPSYS CRITICAL - activity", /* bytes= */ 5000);
    SectionExists("DUMPSYS - activity", /* bytes= */ 10000);
}

TEST_F(BugreportSectionTest, CpuinfoSectionGenerated) {
    SectionExists("DUMPSYS CRITICAL - cpuinfo", /* bytes= */ 1000);
}

TEST_F(BugreportSectionTest, WindowSectionGenerated) {
    SectionExists("DUMPSYS CRITICAL - window", /* bytes= */ 20000);
}

TEST_F(BugreportSectionTest, ConnectivitySectionsGenerated) {
    SectionExists("DUMPSYS HIGH - connectivity", /* bytes= */ 5000);
    SectionExists("DUMPSYS - connectivity", /* bytes= */ 5000);
}

TEST_F(BugreportSectionTest, MeminfoSectionGenerated) {
    SectionExists("DUMPSYS HIGH - meminfo", /* bytes= */ 100000);
}

TEST_F(BugreportSectionTest, BatteryStatsSectionGenerated) {
    SectionExists("DUMPSYS - batterystats", /* bytes= */ 1000);
}

TEST_F(BugreportSectionTest, WifiSectionGenerated) {
    SectionExists("DUMPSYS - wifi", /* bytes= */ 100000);
}

}  // namespace dumpstate
}  // namespace os
}  // namespace android
