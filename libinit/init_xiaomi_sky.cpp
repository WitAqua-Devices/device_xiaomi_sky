/*
 * Copyright (C) 2021-2022 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>

#include <string>

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <libinit_utils.h>

#include "vendor_init.h"

using android::base::GetProperty;

// One vendor image serves every sky board. Stock tells them apart the same
// way: the bootloader passes androidboot.boardid, and
//
//   odm/etc/build_${ro.boot.boardid}.prop       brand, device, model, name
//   vendor/etc/init/init_nfc_sn1xxx.rc          NFC / eSE SKU
//
// The product name in those files is not a property of the board. Each
// regional ROM fills it in for the boards it is meant for and leaves the rest
// empty, so the same S88018EA1 is sky_in on an Indian ROM, sky_global on a
// global one and sky_jp on a Japanese one. ro.boot.hwc is the closest thing
// to "which ROM would stock run here", so the region is taken from it.

namespace {

struct board_info {
    const char* boardid;

    const char* brand;
    const char* device;
    const char* model;
    const char* marketname;

    // Product name on non-Chinese ROMs, before the region suffix, and on
    // the Chinese ROM. nullptr when that ROM leaves it empty.
    const char* name_base;
    const char* name_cn;

    // ODM VINTF / permissions SKU, see ODM_MANIFEST_SKUS. nullptr for boards
    // without NFC.
    const char* sku;
    const char* se_type;
    const char* nfc_config;
};

// S88029JA1 is shared by the SIM-free 23076RA4BR, the SoftBank A401XM and,
// according to KDDI's own ROM, some XIG03 units. Nothing in the bootconfig
// tells them apart (stock relies on which carrier ROM is flashed), so it is
// treated as the SIM-free model.
// clang-format off
const board_info kBoards[] = {
    // boardid      brand    device   model         marketname        base      cn       sku           se_type          nfc_config
    {"S88018AA1",   "Redmi", "sky",   "23076RA4BC", "Redmi Note 12R", nullptr,  "sky",   nullptr,      nullptr,         nullptr},
    {"S88019BA1",   "Redmi", "sky",   "23076RA4BC", "Redmi Note 12R", nullptr,  "sky",   nullptr,      nullptr,         nullptr},
    {"S88018OA1",   "Redmi", "sky",   "23076RA4BC", "Redmi 12R",      nullptr,  "sky2",  nullptr,      nullptr,         nullptr},
    {"S88019OA1",   "Redmi", "sky",   "23076RA4BC", "Redmi 12R",      nullptr,  "sky2",  nullptr,      nullptr,         nullptr},
    {"S88018EA1",   "Redmi", "sky",   "23076RN4BI", "Redmi 12 5G",    "sky",    nullptr, nullptr,      nullptr,         nullptr},
    {"S88019EA1",   "Redmi", "sky",   "23076RN4BI", "Redmi 12 5G",    "sky",    nullptr, nullptr,      nullptr,         nullptr},
    {"S88018XA1",   "POCO",  "sky",   "23076PC4BI", "POCO M6 Pro 5G", "sky_p",  nullptr, nullptr,      nullptr,         nullptr},
    {"S88019EP1",   "POCO",  "sky",   "23076PC4BI", "POCO M6 Pro 5G", "sky_p",  nullptr, nullptr,      nullptr,         nullptr},
    {"S88018PA1",   "Redmi", "river", "23077RABDC", "Redmi 12 5G",    "river",  "river", "hcesimese",  "eSE,HCE,UICC",  "libnfc-qrd-SN100_GLB.conf"},
    {"S88019BP1",   "Redmi", "river", "23077RABDC", "Redmi 12 5G",    "river",  "river", "hcesimese",  "eSE,HCE,UICC",  "libnfc-qrd-SN100_CN.conf"},
    {"S88029AA1",   "Redmi", "river", "23076RN8DY", "Redmi 12 5G",    "river",  nullptr, "hcesim",     "HCE,UICC",      "libnfc-qrd-SN100_GLB.conf"},
    {"S88029JA1",   "Redmi", "river", "23076RA4BR", "Redmi 12 5G",    "river",  nullptr, "hcesim1ese", "eSE,HCE,UICC",  "libnfc-qrd-SN100_JP.conf"},
    {"S88029EA1",   "Redmi", "XIG03", "XIG03",      "Redmi 12 5G",    nullptr,  nullptr, "hcesim1ese", "eSE,HCE,UICC",  "libnfc-qrd-SN100_JP.conf"},
};
// clang-format on

struct region_info {
    const char* hwc;
    const char* suffix;
    const char* mod_device;
    const char* incremental;
};

// Latest stock release of each regional ROM. All of them are Android 15
// with the same system build id.
constexpr const char* kBuildId = "15/AQ3A.240912.001";

const region_info kRegions[] = {
    {"CN",     "",        "sky",           "OS2.0.205.0.VMWCNXM"},
    {"India",  "_in",     "sky_in_global", "OS2.0.204.0.VMWINXM"},
    {"Japan",  "_jp",     "sky_jp_global", "OS2.0.204.0.VMWJPXM"},
    {"Global", "_global", "sky_global",    "OS2.0.210.0.VMWMIXM"},
};

// The au model never runs the SIM-free Japanese ROM. KDDI ships its own,
// delivered as a Google OTA.
constexpr const char* kKddiName = "XIG03_jp_kdi";
constexpr const char* kKddiModDevice = "sky_jp_kd_global";
constexpr const char* kKddiFingerprint =
        "Redmi/XIG03_jp_kdi/XIG03:15/AQ3A.240912.001/OS2.0.11.0.VMWJPKD:user/release-keys";

const board_info* find_board(const std::string& boardid) {
    for (const auto& board : kBoards) {
        if (boardid == board.boardid) return &board;
    }
    return nullptr;
}

const region_info& find_region(const std::string& hwc) {
    for (const auto& region : kRegions) {
        if (hwc == region.hwc) return region;
    }
    // EEA, Taiwan and the rest of the world all report Global.
    return kRegions[3];
}

void set_sku(const board_info& board) {
    if (!board.sku) return;

    std::string sku = board.sku;
    // S88029AA1 is sold both single and dual SIM. Stock gives the single SIM
    // one its own SKU without an NFC config file, keyed on the multisim config
    // the bootloader passes (init.target.rc copies it to
    // persist.radio.multisim.config).
    if (sku == "hcesim" && GetProperty("ro.boot.multisim", "dsds") == "ssss") {
        property_override("ro.boot.product.hardware.sku", "hcesim1");
        property_override("ro.vendor.se.type", board.se_type);
        return;
    }

    property_override("ro.boot.product.hardware.sku", sku);
    property_override("ro.vendor.se.type", board.se_type);
    if (std::string(board.se_type).find("eSE") != std::string::npos) {
        property_override("ro.vendor.se.chip.model", "SN100T");
    }
    property_override("persist.vendor.nfc.config_file_name", board.nfc_config);
}

}  // namespace

void vendor_load_properties() {
    const auto boardid = GetProperty("ro.boot.boardid", "");
    const auto* board = find_board(boardid);
    if (!board) {
        LOG(WARNING) << "Unknown board " << boardid << ", keeping build defaults";
        return;
    }

    set_sku(*board);

    set_ro_build_prop("brand", board->brand, true);
    set_ro_build_prop("device", board->device, true);
    set_ro_build_prop("model", board->model, true);
    set_ro_build_prop("marketname", board->marketname, true);
    property_override("vendor.usb.product_string", board->marketname);
    property_override("bluetooth.device.default_name", board->marketname);

    std::string name, mod_device, fingerprint;
    if (std::string(board->device) == "XIG03") {
        name = kKddiName;
        mod_device = kKddiModDevice;
        fingerprint = kKddiFingerprint;
    } else {
        const auto hwc = GetProperty("ro.boot.hwc", "");
        const auto& region = find_region(hwc);
        // Chinese-only boards keep their Chinese name wherever they are.
        if (board->name_cn && (!board->name_base || std::string(region.hwc) == "CN")) {
            name = board->name_cn;
            mod_device = "sky";
            fingerprint = std::string(board->brand) + "/" + name + "/" + board->device + ":" +
                          kBuildId + "/" + kRegions[0].incremental + ":user/release-keys";
        } else {
            name = std::string(board->name_base) + region.suffix;
            mod_device = region.mod_device;
            fingerprint = std::string(board->brand) + "/" + name + "/" + board->device + ":" +
                          kBuildId + "/" + region.incremental + ":user/release-keys";
        }
    }

    set_ro_build_prop("name", name, true);
    set_ro_build_prop("mod_device", mod_device, true);

    // Recovery reports the build it is part of.
    if (access("/system/bin/recovery", F_OK) == 0) return;

    set_ro_build_prop("fingerprint", fingerprint);
    property_override("ro.bootimage.build.fingerprint", fingerprint);
    property_override("ro.build.description", fingerprint_to_description(fingerprint));
}
