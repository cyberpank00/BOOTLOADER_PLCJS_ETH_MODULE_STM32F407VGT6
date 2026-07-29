<#
    make_metadata.ps1 — build a valid bootloader metadata_t image for offline
    (ST-Link) seeding so the bootloader accepts a directly-flashed application.

    Produces a 340-byte binary matching metadata_t (metadata.h) with:
      app_valid = 1, product_id/hw_revision/app_image_size/app_image_crc32 set,
      trailing crc32 computed over the first 336 bytes (same IEEE-802.3 CRC as
      the firmware's crc32_calc: poly 0xEDB88320, init/xorout 0xFFFFFFFF).

    CRC + buffer construction are done in C# (Add-Type) to avoid Windows
    PowerShell 5.1 signed-integer arithmetic bugs with 32-bit unsigned values.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$AppBin,
    [Parameter(Mandatory=$true)][string]$OutBin,
    [string]$ProductId  = "0x504C1202",
    [string]$HwRevision = "0x0101",
    [string]$FwVersion  = "0x0101"
)

$ErrorActionPreference = "Stop"

Add-Type -TypeDefinition @"
using System;
using System.IO;
public static class MetaGen {
    static uint[] Table() {
        uint[] t = new uint[256];
        for (uint i = 0; i < 256; i++) {
            uint c = i;
            for (int k = 0; k < 8; k++)
                c = ((c & 1) != 0) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }
    public static uint Crc32(byte[] d, int len) {
        uint[] t = Table();
        uint crc = 0xFFFFFFFFu;
        for (int i = 0; i < len; i++)
            crc = t[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
        return crc ^ 0xFFFFFFFFu;
    }
    public static uint Build(string appBin, string outBin, uint productId, ushort hwRev, uint fwVer) {
        byte[] app = File.ReadAllBytes(appBin);
        uint appSize = (uint)app.Length;
        uint appCrc  = Crc32(app, app.Length);
        byte[] buf = new byte[340];
        BitConverter.GetBytes((uint)0x4D455441u).CopyTo(buf, 0);   // magic "META"
        BitConverter.GetBytes((ushort)1).CopyTo(buf, 4);           // struct_version
        BitConverter.GetBytes((uint)8u).CopyTo(buf, 8);            // boot_state = READY_TO_BOOT
        buf[16] = 1;                                               // app_valid
        BitConverter.GetBytes(fwVer).CopyTo(buf, 28);              // fw_version
        BitConverter.GetBytes(productId).CopyTo(buf, 32);          // product_id
        BitConverter.GetBytes(hwRev).CopyTo(buf, 36);              // hw_revision
        BitConverter.GetBytes(fwVer).CopyTo(buf, 48);              // app_fw_version
        BitConverter.GetBytes(appSize).CopyTo(buf, 52);            // app_image_size
        BitConverter.GetBytes(appCrc).CopyTo(buf, 56);             // app_image_crc32
        uint metaCrc = Crc32(buf, 336);
        BitConverter.GetBytes(metaCrc).CopyTo(buf, 336);           // crc32 over first 336
        File.WriteAllBytes(outBin, buf);
        Console.WriteLine("AppBin      : " + appBin);
        Console.WriteLine("AppSize     : " + appSize + " (0x" + appSize.ToString("X") + ")");
        Console.WriteLine("AppCRC32    : 0x" + appCrc.ToString("X8"));
        Console.WriteLine("ProductID   : 0x" + productId.ToString("X8"));
        Console.WriteLine("HwRevision  : 0x" + hwRev.ToString("X4"));
        Console.WriteLine("MetaCRC32   : 0x" + metaCrc.ToString("X8"));
        Console.WriteLine("MetadataOut : " + outBin + " (340 bytes)");
        return metaCrc;
    }
}
"@

$pid32 = [Convert]::ToUInt32($ProductId, 16)
$hw16  = [Convert]::ToUInt16($HwRevision, 16)
$fw32  = [Convert]::ToUInt32($FwVersion, 16)
[void][MetaGen]::Build($AppBin, $OutBin, $pid32, $hw16, $fw32)
