/*
 * kernel_xbox.c - Xbox Identity & Hardware Stubs
 *
 * Provides Xbox-specific exported data (hardware info, kernel version,
 * keys, image filename) and section loading stubs.
 *
 * Most Xbox-specific hardware data is stubbed with plausible retail
 * values. Crypto keys are zeroed since we don't need Xbox Live or
 * EEPROM-based encryption on PC.
 */

#include "kernel.h"
#include "apu.h"                /* mcpx_apu_xc_audio: XC_AUDIO */
#include "recomp_env.h"         /* RENV_GAME_LANGUAGE, RENV_LANGUAGE: XC_LANGUAGE */
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Exported Data Objects
 *
 * These are global variables exported by the Xbox kernel at known ordinals.
 * Game code accesses them directly via the thunk table.
 * ============================================================================ */

/* Hardware info - default report is a standard retail Xbox.
 * A target may override via xbox_kernel_set_version() or by patching the
 * global directly; no title-specific values are baked in. */
XBOX_HARDWARE_INFO xbox_HardwareInfo = {
    .Flags       = 0x00000020,  /* Retail Xbox */
    .GpuRevision = 0xD2,       /* NV2A D2 revision */
    .McpRevision = 0xD4,       /* MCPX D4 revision */
    .Reserved    = {0, 0}
};

/* Kernel version - default is a stock retail value. Titles built against a
 * different XDK can report their own through xbox_kernel_set_version(). */
XBOX_KRNL_VERSION xbox_KrnlVersion = {
    .Major = 1,
    .Minor = 0,
    .Build = 5849,
    .Qfe   = 1
};

/* Override the kernel version reported to game code (ordinal 324).
 * Match the XDK revision the target title was built with if it checks it. */
void xbox_kernel_set_version(USHORT major, USHORT minor, USHORT build, USHORT qfe)
{
    xbox_KrnlVersion.Major = major;
    xbox_KrnlVersion.Minor = minor;
    xbox_KrnlVersion.Build = build;
    xbox_KrnlVersion.Qfe   = qfe;
}

/* Crypto keys - zeroed, not needed for PC operation.
 * These are unique per-console on real hardware. */
UCHAR xbox_EEPROMKey[16]              = {0};
UCHAR xbox_HDKey[16]                  = {0};
UCHAR xbox_SignatureKey[16]           = {0};
UCHAR xbox_LANKey[16]                 = {0};
UCHAR xbox_AlternateSignatureKeys[16][16] = {{0}};

/* Public key data for Xbox Live signature verification - not needed */
UCHAR xbox_XePublicKeyData[284] = {0};

/* Image filename - the XBE path as seen by the kernel */
static char g_image_filename[] = "\\Device\\CdRom0\\default.xbe";
XBOX_ANSI_STRING xbox_XeImageFileName = {
    .Length        = sizeof(g_image_filename) - 1,
    .MaximumLength = sizeof(g_image_filename),
    .Buffer        = g_image_filename
};

/* Launch data page - used for title-to-title launches (e.g., Xbox Dashboard → game).
 * Allocated dynamically and zeroed for normal game boot. */
static XBOX_LAUNCH_DATA_PAGE g_launch_data_page = {0};
XBOX_LAUNCH_DATA_PAGE* xbox_LaunchDataPage = &g_launch_data_page;

/* ============================================================================
 * Section Loading
 *
 * XeLoadSection/XeUnloadSection manage on-demand loading of XBE sections.
 * On Xbox, some sections are demand-paged from disc. In our recompilation,
 * the entire executable is loaded into memory, so these are reference-counting
 * no-ops.
 * ============================================================================ */

NTSTATUS __stdcall xbox_XeLoadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    InterlockedIncrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeLoadSection: '%s' at %p (size=%u, refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        Section->VirtualAddress, Section->VirtualSize,
        Section->SectionReferenceCount);

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_XeUnloadSection(PXBE_SECTION_HEADER Section)
{
    if (!Section)
        return STATUS_INVALID_PARAMETER;

    LONG new_count = InterlockedDecrement(&Section->SectionReferenceCount);

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "XeUnloadSection: '%s' (refcount=%d)",
        Section->SectionName ? Section->SectionName : "<null>",
        new_count);

    return STATUS_SUCCESS;
}

/* ============================================================================
 * EEPROM / Non-Volatile Settings
 *
 * ExQueryNonVolatileSetting / ExSaveNonVolatileSetting read and write EEPROM
 * settings. On real hardware these are stored in the 256-byte EEPROM on the
 * SMBus. For recompilation, we return sensible defaults:
 *   - Region: North America
 *   - Video: none of widescreen/480p/720p/1080i unless set (kernel.h,
 *     xbox_SetVideoSettings)
 *   - Language: English, or game.language (xbox_GetLanguage)
 *   - Audio: the APU's speaker layout, stereo or 5.1 (Dolby Digital)
 *   - DVD Region: Region 1 (North America)
 * ============================================================================ */

const char *const xbox_language_names[XC_LANGUAGE_COUNT + 1] = {
    "english", "japanese", "german", "french", "spanish", "italian",
    "korean", "chinese", "portuguese", NULL
};

/* game.language: a name above or its number; English when unset or not
 * one of them. Read on each query: the title asks early in boot, and a
 * title that lacks the language's files may set the variable back before
 * that (recomp_env_set). */
ULONG xbox_GetLanguage(void)
{
    const char *v = recomp_env(RENV_GAME_LANGUAGE);
    ULONG i;

    /* system.language / RECOMP_LANGUAGE, antbox's name for the same thing
     * (Double-S.T.E.A.L 2's enhance.toml), where game.language is unset. */
    if (!v || !*v)
        v = recomp_env(RENV_LANGUAGE);

    if (!v || !*v)
        return XC_LANGUAGE_ENGLISH;
    {
        char low[16];
        for (i = 0; v[i] && i < sizeof low - 1; i++)    /* any case, as RECOMP_LANGUAGE took */
            low[i] = (char)(v[i] >= 'A' && v[i] <= 'Z' ? v[i] - 'A' + 'a' : v[i]);
        low[i] = 0;
        for (i = 0; i < XC_LANGUAGE_COUNT; i++)
            if (!strcmp(low, xbox_language_names[i]))
                return i + 1;
    }
    i = (ULONG)strtoul(v, NULL, 10);
    return i >= 1 && i <= XC_LANGUAGE_COUNT ? i : XC_LANGUAGE_ENGLISH;
}

/* Minutes east of UTC on the host now, daylight saving included. */
static LONG host_utc_offset_minutes(void)
{
#if defined(_WIN32)
    TIME_ZONE_INFORMATION tzi;
    DWORD r = GetTimeZoneInformation(&tzi);
    LONG bias = tzi.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias
                         : r == TIME_ZONE_ID_STANDARD ? tzi.StandardBias : 0);
    return -bias;
#else
    time_t now = time(NULL);
    struct tm lt;
    if (!localtime_r(&now, &lt))
        return 0;
    return (LONG)(lt.tm_gmtoff / 60);
#endif
}

NTSTATUS __stdcall xbox_ExQueryNonVolatileSetting(
    ULONG ValueIndex, PULONG Type, PVOID Value, ULONG ValueLength, PULONG ResultLength)
{
    if (!Value)
        return STATUS_INVALID_PARAMETER;

    xbox_log(XBOX_LOG_DEBUG, XBOX_LOG_XBOX,
        "ExQueryNonVolatileSetting: index=0x%02X len=%u", ValueIndex, ValueLength);

    switch (ValueIndex) {
    case XC_LANGUAGE:
        /* game.language (or antbox's system.language): the dashboard's codes,
         * English 1 (default), Japanese 2, German 3, French 4, Spanish 5,
         * Italian 6, Korean 7, Chinese 8, Portuguese 9. Double-S.T.E.A.L 2
         * carries English and Japanese data and picks by it. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = xbox_GetLanguage();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_VIDEO:
        /* The dashboard's video settings, XC_VIDEO_AV_* bits (kernel.h):
         * stock none, else display.widescreen / display.hdtv. */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = xbox_GetVideoSettings();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_AUDIO:
        /* The dashboard's audio setting, the layout the APU mixes for
         * (RECOMP_AUDIO_SPEAKERS): stereo 0, or for 5.1 SURROUND |
         * ENABLE_AC3 (0x00010002), "Dolby Digital" ticked. XAPI's
         * XGetAudioFlags passes it on unchanged on our AV pack (it forces
         * mono on the RF pack, clears the AC-3/DTS bits on the composite
         * one: Burnout 3's sub_001D00F2), and RenderWare's DirectSound
         * driver takes 5.1 only with both bits (sub_0020A120: surround
         * alone is its matrixed mode 4, with AC-3 mode 5). No AC-3 is
         * encoded: the six feeds leave as LPCM (apu_speakers.c). */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = mcpx_apu_xc_audio();
            if (Type) *Type = 4; /* REG_DWORD */
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_P_CONTROL_GAMES:
    case XC_P_CONTROL_MOVIES:
        /* Unrestricted. Titles compare their XBE certificate's rating against
         * this and refuse to run (Halo boots to the dashboard) if the console
         * is configured more strictly, so 0 means "no restriction". */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_DVD_REGION:
        /* Region 1 (North America) */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 1;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_MISC:
        /* Misc flags: 0 = no special flags */
        if (ValueLength >= sizeof(ULONG)) {
            *(PULONG)Value = 0;
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(ULONG);
        }
        break;

    case XC_TIMEZONE_BIAS:
        /* The host's time zone as the dashboard keeps it, Win32's sign:
         * minutes WEST of UTC, UTC = local + bias (Arizona 420; it was
         * -300 on every host, US Eastern with the sign turned). The
         * daylight-saving fields stay zero, so the offset in force when
         * the title asks is the whole answer. XAPI reads it in XC_MAX_OS. */
        if (ValueLength >= sizeof(LONG)) {
            *(PLONG)Value = -host_utc_offset_minutes();
            if (Type) *Type = 4;
            if (ResultLength) *ResultLength = sizeof(LONG);
        }
        break;

    case XC_TZ_STD_NAME:
    case XC_TZ_DLT_NAME:
        /* Four characters, not terminated ("MST"): the host's own. */
        if (ValueLength >= 4) {
            const char *nm = "UTC";
#if !defined(_WIN32)
            tzset();
            nm = tzname[ValueIndex == XC_TZ_DLT_NAME ? 1 : 0];
#endif
            memset(Value, 0, 4);
            memcpy(Value, nm, strnlen(nm, 4));
            if (Type) *Type = 3; /* REG_BINARY */
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_TZ_STD_DATE:
    case XC_TZ_DLT_DATE:
    case XC_TZ_STD_BIAS:
    case XC_TZ_DLT_BIAS:
    case XC_P_CONTROL_PASSWORD:
    case XC_ONLINE_IP_ADDRESS:
    case XC_ONLINE_DNS_ADDRESS:
    case XC_ONLINE_DEFAULT_GATEWAY:
    case XC_ONLINE_SUBNET_MASK:
        /* No daylight-saving rule (XC_TIMEZONE_BIAS is the offset in force
         * now), no password, no static network settings. */
        if (ValueLength >= 4) {
            memset(Value, 0, 4);
            if (Type) *Type = ValueIndex <= XC_TZ_DLT_DATE ? 3 : 4;
            if (ResultLength) *ResultLength = 4;
        }
        break;

    case XC_MAX_OS: {
        /* The dashboard's whole user section (XBOX_USER_SETTINGS, the
         * EEPROM from 0x60, 0x60 bytes), which XAPI reads at once for its
         * time zone (GetLocalTime, FileTimeToLocalFileTime) rather than
         * XC_TIMEZONE_BIAS. It was all zeros: Double-S.T.E.A.L 2 dated its
         * saves and replays in UTC. Each field is the answer its own index
         * gives; the checksum is the EEPROM's (one's-complement sum of the
         * dwords, carries folded back in). */
        static const uint8_t field[0x13] = {
            0x04, 0x08, 0x0C, 0x18, 0x1C, 0x28, 0x2C, 0x30, 0x34, 0x38,
            0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C,
        };
        uint8_t us[0x60];
        uint64_t sum = 0;
        ULONG i, t, n;
        if (ValueLength < sizeof us) {
            if (ResultLength) *ResultLength = sizeof us;
            return (NTSTATUS)0xC0000023L;   /* STATUS_BUFFER_TOO_SMALL */
        }
        memset(us, 0, sizeof us);
        for (i = 0; i < sizeof field; i++)
            xbox_ExQueryNonVolatileSetting(i, &t, us + field[i], 4, &n);
        for (i = 4; i < sizeof us; i += 4) {
            uint32_t w;
            memcpy(&w, us + i, 4);
            sum += w;
        }
        while (sum >> 32)
            sum = (sum & 0xFFFFFFFFu) + (sum >> 32);
        {
            uint32_t ck = ~(uint32_t)sum;
            memcpy(us, &ck, 4);
        }
        memcpy(Value, us, sizeof us);
        if (Type) *Type = 3; /* REG_BINARY */
        if (ResultLength) *ResultLength = sizeof us;
        break;
    }

    default:
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_XBOX,
            "ExQueryNonVolatileSetting: unhandled index 0x%02X", ValueIndex);
        memset(Value, 0, ValueLength);
        if (Type) *Type = 4;
        if (ResultLength) *ResultLength = ValueLength;
        break;
    }

    return STATUS_SUCCESS;
}

NTSTATUS __stdcall xbox_ExSaveNonVolatileSetting(
    ULONG ValueIndex, ULONG Type, PVOID Value, ULONG ValueLength)
{
    (void)Type;
    (void)Value;
    (void)ValueLength;

    xbox_log(XBOX_LOG_INFO, XBOX_LOG_XBOX,
        "ExSaveNonVolatileSetting: index=0x%02X len=%u (ignored - read-only on PC)",
        ValueIndex, ValueLength);

    /* Accept writes silently but don't persist them */
    return STATUS_SUCCESS;
}

/* ============================================================================
 * Network / PHY
 *
 * PhyGetLinkState reports Ethernet link status: XNET_ETHERNET_LINK_ACTIVE
 * (0x01) and the speed and duplex bits, or 0 for no cable.
 *
 * There is no NIC here, so the answer is "no cable", as on a console with
 * nothing plugged in and in xemu with networking off (its default). Reporting
 * a link sent XNet into DHCP on a NIC that never answers: Burnout 3 polls
 * XNetGetTitleXnAddr on its boot loading screen until that gives up, and the
 * bar sat near the end for seconds (BO-164). With no link XNet reports it at
 * once and the title goes on.
 * ============================================================================ */

ULONG __stdcall xbox_PhyGetLinkState(BOOLEAN Verify)
{
    (void)Verify;
    return 0; /* no link */
}

NTSTATUS __stdcall xbox_PhyInitialize(BOOLEAN ForceReset, PVOID Param2)
{
    (void)ForceReset;
    (void)Param2;
    return STATUS_SUCCESS;
}
