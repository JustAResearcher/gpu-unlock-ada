// SPDX-License-Identifier: GPL-2.0-only
// Windows user-mode firmware variable access. No driver is installed.
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.IO;
using Microsoft.Win32.SafeHandles;

namespace GpuUnlock.Session
{
    public sealed class FirmwareValue
    {
        public byte[] Bytes { get; private set; }
        public uint Attributes { get; private set; }
        public FirmwareValue(byte[] bytes, uint attributes) { Bytes = bytes; Attributes = attributes; }
    }

    public static class FirmwareVariables
    {
        private const string GlobalGuid = "{8BE4DF61-93CA-11D2-AA0D-00E098032B8C}";
        [StructLayout(LayoutKind.Sequential)] private struct Luid { public uint Low; public int High; }
        [StructLayout(LayoutKind.Sequential)] private struct TokenPrivileges
        { public uint Count; public Luid Luid; public uint Attributes; }
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetFirmwareEnvironmentVariableExW(string name, string guid, byte[] buffer, uint size, out uint attributes);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetFirmwareEnvironmentVariableExW(string name, string guid, byte[] buffer, uint size, uint attributes);
        [DllImport("kernel32.dll")] private static extern IntPtr GetCurrentProcess();
        [DllImport("kernel32.dll", SetLastError = true)] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool LookupPrivilegeValue(string system, string name, out Luid luid);
        [DllImport("advapi32.dll", SetLastError = true)]
        private static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges requested,
            uint bufferLength, out TokenPrivileges previous, out uint returnLength);

        private sealed class Privilege : IDisposable
        {
            private IntPtr token;
            private TokenPrivileges previous;
            private bool enabled;
            internal Privilege()
            {
                if (!OpenProcessToken(GetCurrentProcess(), 0x20 | 0x8, out token))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
                try
                {
                    Luid luid;
                    if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    var requested = new TokenPrivileges { Count = 1, Luid = luid, Attributes = 2 };
                    uint length;
                    bool ok = AdjustTokenPrivileges(token, false, ref requested,
                        (uint)Marshal.SizeOf(typeof(TokenPrivileges)), out previous, out length);
                    int error = Marshal.GetLastWin32Error();
                    if (!ok || error != 0) throw new Win32Exception(error);
                    enabled = true;
                }
                catch { CloseHandle(token); token = IntPtr.Zero; throw; }
            }
            public void Dispose()
            {
                if (token == IntPtr.Zero) return;
                try
                {
                    if (enabled)
                    {
                        TokenPrivileges ignored; uint length;
                        if (!AdjustTokenPrivileges(token, false, ref previous,
                            (uint)Marshal.SizeOf(typeof(TokenPrivileges)), out ignored, out length))
                            throw new Win32Exception(Marshal.GetLastWin32Error());
                    }
                }
                finally { CloseHandle(token); token = IntPtr.Zero; }
            }
        }

        private static void CheckName(string name, bool writing)
        {
            // Deliberately no write capability for BootOrder, SecureBoot, keys, or BCD.
            if (name == "BootNext" || Regex.IsMatch(name ?? "", "\\ABoot[0-9A-F]{4}\\z")) return;
            if (!writing && (name == "BootOrder" || name == "BootCurrent")) return;
            throw new ArgumentException("Only session boot variables are accepted.", "name");
        }
        public static FirmwareValue Read(string name)
        {
            CheckName(name, false);
            using (new Privilege())
            {
                // A bounded buffer is larger than any option this application creates.
                var buffer = new byte[65536]; uint attributes;
                uint count = GetFirmwareEnvironmentVariableExW(name, GlobalGuid, buffer, (uint)buffer.Length, out attributes);
                if (count == 0)
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error == 203) return null; // ERROR_ENVVAR_NOT_FOUND only.
                    throw new Win32Exception(error, "Cannot read firmware variable " + name);
                }
                var result = new byte[count]; Array.Copy(buffer, result, count);
                return new FirmwareValue(result, attributes);
            }
        }
        public static void Write(string name, byte[] value)
        {
            CheckName(name, true);
            if (value == null || value.Length == 0 || value.Length > 65535)
                throw new ArgumentException("A nonempty bounded value is required.", "value");
            if (name == "BootNext" && value.Length != 2) throw new ArgumentException("BootNext is UINT16.");
            using (new Privilege())
                if (!SetFirmwareEnvironmentVariableExW(name, GlobalGuid, value, (uint)value.Length, 7))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot write firmware variable " + name);
        }
        public static void Delete(string name)
        {
            CheckName(name, true);
            using (new Privilege())
                if (!SetFirmwareEnvironmentVariableExW(name, GlobalGuid, null, 0, 7))
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Cannot remove firmware variable " + name);
        }
    }

    // Apply the journal's owner and DACL atomically at creation. Existing paths
    // are validated by the caller; they are never silently taken over.
    public static class ProtectedJournal
    {
        private const string Descriptor = "O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
        [StructLayout(LayoutKind.Sequential)] private struct SecurityAttributes
        { public uint Length; public IntPtr Descriptor; public int Inherit; }
        [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool ConvertStringSecurityDescriptorToSecurityDescriptorW(string text, uint revision, out IntPtr result, out uint size);
        [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr value);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateDirectoryW(string path, ref SecurityAttributes attributes);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFileW(string path, uint access, uint share, ref SecurityAttributes attributes,
            uint disposition, uint flags, IntPtr template);
        private static SecurityAttributes Security()
        {
            IntPtr descriptor; uint size;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(Descriptor, 1, out descriptor, out size))
                throw new Win32Exception(Marshal.GetLastWin32Error());
            return new SecurityAttributes { Length = (uint)Marshal.SizeOf(typeof(SecurityAttributes)), Descriptor = descriptor, Inherit = 0 };
        }
        public static void CreateDirectory(string path)
        {
            var attributes = Security();
            try
            {
                if (!CreateDirectoryW(path, ref attributes))
                {
                    int error = Marshal.GetLastWin32Error();
                    if (error != 183) throw new Win32Exception(error);
                }
            }
            finally { LocalFree(attributes.Descriptor); }
        }
        public static void WriteNewFile(string path, byte[] bytes)
        {
            var attributes = Security();
            try
            {
                IntPtr raw = CreateFileW(path, 0x40000000, 0, ref attributes, 1, 0x80000080, IntPtr.Zero);
                if (raw == new IntPtr(-1)) throw new Win32Exception(Marshal.GetLastWin32Error());
                using (var handle = new SafeFileHandle(raw, true))
                using (var file = new FileStream(handle, FileAccess.Write))
                { file.Write(bytes, 0, bytes.Length); file.Flush(true); }
            }
            finally { LocalFree(attributes.Descriptor); }
        }
    }
}
