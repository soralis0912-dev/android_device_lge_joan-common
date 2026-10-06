#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from extract_utils.fixups_blob import (
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixup_remove,
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)
from extract_utils.utils import (
    run_cmd,
)

import os
import zipfile

namespace_imports = [
    "device/lge/joan-common",
    "hardware/lge",
    "hardware/qcom-caf/common/libqti-perfd-client",
    "hardware/qcom-caf/msm8998",
    "hardware/qcom-caf/wlan",
    "vendor/qcom/opensource/dataservices",
    "vendor/qcom/opensource/display",
]


def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


def blob_fixup_drop_native_libs(ctx, file, file_path: str, *args, **kwargs):
    """Rewrite an apk without its lib/ entries."""
    stripped = f'{file_path}.nolib'
    with zipfile.ZipFile(file_path) as src, zipfile.ZipFile(stripped, 'w') as dst:
        for info in src.infolist():
            if not info.filename.startswith('lib/'):
                dst.writestr(info, src.read(info))
    os.replace(stripped, file_path)


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'com.qualcomm.qti.dpm.api@1.0',
        'vendor.qti.hardware.fm@1.0',
        'vendor.qti.hardware.qccsyshal@1.0',
        'vendor.qti.hardware.qccvndhal@1.0',
        'vendor.qti.imsrtpservice@3.0'
    ): lib_fixup_vendor_suffix,
}


blob_fixups: blob_fixups_user_type = {
    # libhidltransport was folded into libhidlbase, and only vendor still gets a
    # compat stub. lgemtvserver runs off product, where there is none.
    'product/bin/lgemtvserver': blob_fixup()
        .remove_needed('libhidltransport.so'),
    # The binary had to move to /system to reach libutils and libbinder, so the
    # path baked into the stock init script has to follow it.
    'product/etc/init/lgemtvserver.rc': blob_fixup()
        .regex_replace('/product/bin/lgemtvserver', '/system/bin/lgemtvserver'),
    # libmediaextractor is gone from the media framework, and LG's DMB MIME
    # constant went with their libstagefright_foundation. libmtvmedia_shim/ has
    # both, under its own name rather than claiming the framework's.
    #
    # The second fixup is the CAS path. android::MemoryHeapBase lost a virtual
    # base since android 9, so its vtable carries one metadata word less and the
    # upcasts this was built with - inlined, with the displacement baked in -
    # read the wrong words:
    #
    #              android 9                          current
    #   vptr-16    vbase offset, RefBase   (0x30)      (before the vtable)
    #   vptr-12    vbase offset, IMemoryHeap (0x20)    vbase offset, RefBase
    #
    # so the first CAS callback takes lgemtvserver down with SIGSEGV. The class
    # layout is not something a shim can paper over, but the blob only ever
    # looks up the constructor by name - the rest goes through the vtable of
    # whatever that constructor built - so pointing it at a MemoryHeapBase that
    # still has the virtual base makes all of it line up again. That class is
    # android::LegacyHeapBase in libmtvmedia_shim/memoryheap_shim.cpp, and this
    # rewrites the one undefined symbol that reaches it. Same length, so it is a
    # byte-for-byte edit of a single .dynstr entry, and .gnu.hash does not index
    # undefined symbols, so no hash bucket has to follow it.
    # The playback path has the same shape of problem twice over, and the same
    # answer: what the blob looks up by name is one constructor, everything
    # after it goes through a vtable, so the constructor decides the layout.
    #
    #   MediaPlayerBase gained setDataSource(const String8&) at slot 13, so the
    #   android 9 slot numbers this drives NuPlayerDriver with are all one
    #   short from setVideoSurfaceTexture on - prepareAsync calls prepare,
    #   start calls prepareAsync, and setAudioSink calls releaseDrm, which is
    #   why the blob's audio output never reached the player.
    #
    #   Its AudioSink is on the other side of the same break: the framework
    #   calls the blob's object, and setPlayerIId() was inserted after open().
    #   That one is bridged inside the shim rather than renamed, because the
    #   vtable being called is the blob's own.
    #
    # libmtvmedia_shim/nuplayer_shim.cpp has both, built against vtables
    # decoded out of the stock libmediaplayerservice.so rather than guessed.
    'system/lib/libmtv_servicejp.lge.so': blob_fixup()
        .replace_needed('libmediaextractor.so', 'libmtvmedia_shim.so')
        .binary_regex_replace(
            rb'_ZN7android14MemoryHeapBaseC1EjjPKc',
            b'_ZN7android14LegacyHeapBaseC1EjjPKc')
        .binary_regex_replace(
            rb'_ZN7android14NuPlayerDriverC1Ei',
            b'_ZN7android14LegacyNuDriverC1Ei')
        # android::AudioTrack went the other way from MemoryHeapBase: it gained
        # a virtual base, so RefBase moved from +0 to +1000 and the class grew
        # from 840 bytes to 1008. The blob allocates the android 9 size and
        # reaches four fields by their android 9 offsets - mFrameCount at +0x48,
        # mChannelCount at +0x64, mFrameSize at +0x1bc and mStatus at +0x1c0,
        # thirteen sites between them - so neither a wrapper nor a placement
        # new into its allocation can hold: the object has to be the shape it
        # thinks it is. audiotrack_shim.cpp is that class.
        #
        # Anchored on "android10AudioTrack" rather than "10AudioTrack", and the
        # difference matters. The blob also carries two weak *definitions*,
        # sp<AudioTrack>::operator=, whose names contain NS_10AudioTrack.
        # Defined symbols are indexed in .gnu.hash - renaming those puts them in
        # the wrong bucket, and since the references to them are weak, bionic
        # resolves to address 0 and calls it. The prefix leaves them alone, and
        # nothing else in the image defines them, so they keep resolving to the
        # blob's own copies. Fourteen undefined symbols match, 19 bytes each way.
        .binary_regex_replace(
            rb'android10AudioTrack',
            b'android10LegacyTrak')
        # MobitPlayerService::selectTrack keeps its two Parcels in function-local
        # statics, and their .bss slots were sized by the android 9
        # sizeof(Parcel) of 52. The class is 60 bytes now, so +52 - which is
        # Parcel::mOwner - lands on the guard variable sitting right behind each
        # object, and __cxa_guard_release stores 1 into it. The first freeData()
        # then calls through mOwner == 1. It is not a corruption that might bite:
        # it is every time, from the first call.
        #
        # Nothing can be freed up in .bss - the linker packed it solid to its
        # last byte, where cch_base_info ends at 0x78232c. What is free is the
        # rest of that page: the RW segment ends mid-page and bionic maps and
        # zeroes through page_end (0x783000), so 0xcd4 bytes there are mapped,
        # writable and claimed by nothing. The objects move to 0x782330 and
        # 0x782370; the guards stay where they are, since their addresses come
        # from literals of their own.
        #
        #   .text 0x49ce4 -> Parcel #1, constructor site
        #   .text 0x49cfc -> Parcel #2, constructor site
        #   .text 0x49d0c -> Parcel #1, the outgoing data parcel
        #   .text 0x49d1c -> Parcel #2, the reply parcel
        #
        # Each is the displacement of an `ldr rX,[pc,#n]; add rX,pc` pair, so the
        # value is target - (address of the add + 4). The window matched here is
        # the whole literal pool - unique in the file, where a bare four-byte
        # displacement would not stay that way - and the six guard literals in it
        # are rewritten to themselves.
        #
        # This has to be sig_replace rather than binary_regex_replace: the bytes
        # include 0x2e and friends, which a regex would read as metacharacters.
        .sig_replace(
            '6c 80 72 00 2c 80 72 00 92 d1 04 00 68 e3 04 00 '
            '46 80 72 00 74 81 72 00 74 80 72 00 32 80 72 00 '
            '60 d1 04 00 36 e3 04 00 4c 80 72 00 fa 80 72 00 '
            '4d 20 02 00 e7 2e 02 00 b8 2e 02 00 e8 80 72 00',
            '6c807200a886730092d1040068e30400'
            '468072007481720074807200b6867300'
            '60d1040036e304004c80720076877300'
            '4d200200e72e0200b82e02006c877300')
        # And the RW PT_LOAD's p_memsz, 0x6eca6c -> 0x6ecaf0, so the two Parcels
        # are inside the segment the file declares rather than in a page that
        # only happens to be mapped. Nothing changes at run time - page_end was
        # already 0x783000 - but the file stops lying about what it uses.
        #
        # Note this pins the program header layout: adding a patchelf-backed
        # fixup to this blob would move it and leave this silently unmatched.
        # replace_needed above does not, because the new name is no longer than
        # the old one and extract-utils rewrites those in place.
        .sig_replace(
            '01 00 00 00 c0 38 09 00 c0 58 09 00 c0 58 09 00 '
            '88 30 00 00 6c ca 6e 00 06 00 00 00 00 10 00 00',
            '01000000c0380900c0580900c0580900'
            '88300000f0ca6e000600000000100000'),
    # LGJDMB reaches into com.lge - the shared resource library that ships as
    # framework/lge-res/lge-res.apk - for dimensions, colours and its activity
    # themes, and it does not name that library in a uses-library, so on stock
    # something in LG's framework must be linking it into every app. Nothing
    # here does, which leaves all 23 of those references as
    # TYPE_DYNAMIC_REFERENCE and kills the app on the first screen it draws:
    #
    #   add_preset_area_list_item -> TextView
    #   UnsupportedOperationException: Can't convert value at index 2 to
    #   dimension: type=0x7
    #
    # and that screen is the area picker, which an unconfigured install goes
    # straight to. patches/LGJDMB/ answers them out of the apk's own package
    # with the values read back out of the stock library, which is a great deal
    # less to carry than a 47 MB resource blob and a framework that links it.
    #
    # The only native code in the apk is ACCESS's two data broadcast (BML)
    # engines, and they are 32-bit, which makes the whole app a 32-bit
    # process. 0004 turns their JNI entry points into Java that reports
    # failure, so the app runs as arm64 with data broadcasting off until the
    # engines have a 64-bit replacement.
    'system/priv-app/LGJDMB/LGJDMB.apk': blob_fixup()
        .apktool_patch('patches/LGJDMB')
        .call(blob_fixup_drop_native_libs)
        .stripzip(),
    # Same for the shared library declaration: the jar moved to /system, and the
    # path in here is what decides which linker namespace the app gets when it
    # loads the jar's JNI - product-clns cannot see libcutils.
    'system/etc/permissions/com.lge.broadcast.jfullseg.xml': blob_fixup()
        .regex_replace('/product/framework/', '/system/framework/'),
    'system_ext/lib64/lib-imscamera.so': blob_fixup()
        .add_needed('libgui_shim.so'),
    'system_ext/lib64/lib-imsvideocodec.so': blob_fixup()
        .add_needed('libgui_shim.so')
        .replace_needed('libqdMetaData.so', 'libqdMetaData.system.so'),
    'vendor/lib/hw/camera.msm8998.so': blob_fixup()
        .remove_needed('libgui.so')
        .remove_needed('libsensor.so')
        .remove_needed('libui.so')
        .binary_regex_replace(b'libandroid.so', b'libui_shim.so'),
    'vendor/lib/libarcsoft_beauty_picselfie.so': blob_fixup()
        .remove_needed('libandroid.so')
        .remove_needed('libjnigraphics.so')
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    'vendor/lib/libfilm_emulation.so': blob_fixup()
        .remove_needed('libjnigraphics.so')
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    'vendor/lib/libmmcamera_bokeh.so': blob_fixup()
        .replace_needed('libui.so', 'libui_shim.so'),
    'vendor/lib/libmpbase.so': blob_fixup()
        .remove_needed('libandroid.so')
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    (
        'vendor/lib/libAutoContrast.so',
        'vendor/lib/libSJFingerDetect.so',
        'vendor/lib/libSJVideoNR.so',
        'vendor/lib/libarcsoft_object_tracking.so',
        'vendor/lib/libarcsoft_picselfie_algorithm.so',
        'vendor/lib/libcinemaeffect.so',
        'vendor/lib/libfilm_emulation_symphony.so',
        'vendor/lib/liblghdri.so',
        'vendor/lib/liblgmda.so',
        'vendor/lib/libmorpho_image_stab31.so',
        'vendor/lib/libmorpho_superzoom.so'
    ): blob_fixup()
        .replace_needed('libstdc++.so', 'libstdc++_vendor.so'),
    'vendor/lib/sensors.ssc.so': blob_fixup()
        .sig_replace('11 F0 9E F8', '4D D2 00 BF'),
    'vendor/lib64/sensors.ssc.so': blob_fixup()
        .sig_replace('F2 8B FF 97', '2A 00 00 14'),
    'vendor/lib64/libwvhidl.so': blob_fixup()
        .add_needed('libcrypto_shim.so'),
    'vendor/lib/libmmcamera_faceproc.so': blob_fixup()
        .clear_symbol_version('__aeabi_memcpy')
        .clear_symbol_version('__aeabi_memset')
        .clear_symbol_version('__gnu_Unwind_Find_exidx'),
}  # fmt: skip

module = ExtractUtilsModule(
    'joan-common',
    'lge',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

if __name__ == '__main__':
    utils = ExtractUtils.device(module)
    utils.run()
