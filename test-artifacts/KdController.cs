using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

// Use the WDK 7.1 debugger engine that speaks Vista's legacy serial KD
// protocol.  The Windows 10 ARM64/x86 engines connect but repeatedly restart
// their version-probe exchange when Vista returns STATE_CHANGE64.  Windows 11
// ARM runs this Microsoft x64 debugger under its built-in emulator.
const string kdPath = @"C:\WDK71\Debuggers\PFiles\Debugging Tools for Windows (x64)\kd.exe";
// The host bridge serializes debugger writes into individual UART bytes.  A
// direct TCP burst can overrun checked Vista's early polled KD receiver while
// QEMU runs under single-core TCG.
const string transport = "com:ipport=2021,port=10.211.55.2";
const string capturePath = @"C:\VistaSigning\vista-kd-capture.log";
const string controllerPath = @"C:\VistaSigning\vista-kd-controller.log";
const string symbols = @"C:\VistaSigning;srv*C:\VistaSigning\symbols*https://msdl.microsoft.com/download/symbols";

using var controllerLog = new StreamWriter(controllerPath, append: false, Encoding.UTF8)
{
    AutoFlush = true
};

void Log(string message) =>
    controllerLog.WriteLine($"{DateTime.UtcNow:O} {message}");

try
{
    File.Delete(capturePath);
}
catch (Exception error)
{
    Log($"initial cleanup warning: {error.Message}");
}

Native.FreeConsole();
if (!Native.AllocConsole())
    throw new InvalidOperationException($"AllocConsole failed: {Marshal.GetLastWin32Error()}");
if (!Native.SetConsoleCtrlHandler(IntPtr.Zero, true))
    throw new InvalidOperationException($"SetConsoleCtrlHandler failed: {Marshal.GetLastWin32Error()}");

var startInfo = new ProcessStartInfo
{
    FileName = kdPath,
    // Attach before PnP without requesting an additional debugger break.
    // Checked display-model diagnostics remain stopped until the deployment
    // orchestrator inspects their stack and explicitly continues them.
    Arguments = $"-k {transport} -t -y \"{symbols}\" -logo \"{capturePath}\"",
    UseShellExecute = false,
    RedirectStandardInput = false,
    RedirectStandardOutput = false,
    RedirectStandardError = false,
    CreateNoWindow = false
};

using var kd = new Process { StartInfo = startInfo, EnableRaisingEvents = true };

if (!kd.Start())
    throw new InvalidOperationException("kd.exe did not start");

Log($"started kd.exe PID {kd.Id}");
Log("controller console is ready");
kd.WaitForExit();

Log($"kd.exe exited with code {kd.ExitCode}");
Environment.ExitCode = kd.ExitCode;

static class Native
{
    public const uint GenericRead = 0x80000000;
    public const uint GenericWrite = 0x40000000;
    public const uint FileShareRead = 0x00000001;
    public const uint FileShareWrite = 0x00000002;
    public const uint OpenExisting = 3;

    [StructLayout(LayoutKind.Sequential)]
    public struct KeyEventRecord
    {
        [MarshalAs(UnmanagedType.Bool)] public bool KeyDown;
        public ushort RepeatCount;
        public ushort VirtualKeyCode;
        public ushort VirtualScanCode;
        public char UnicodeChar;
        public uint ControlKeyState;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct InputRecord
    {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KeyEventRecord KeyEvent;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool FreeConsole();

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool AllocConsole();

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateFile(
        string fileName, uint desiredAccess, uint shareMode,
        IntPtr securityAttributes, uint creationDisposition,
        uint flagsAndAttributes, IntPtr templateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool WriteConsoleInput(
        IntPtr consoleInput, [In] InputRecord[] buffer,
        uint length, out uint written);

    public static void SendConsoleInput(IntPtr input, string text)
    {
        var records = new InputRecord[text.Length * 2];
        int index = 0;
        foreach (char character in text)
        {
            ushort virtualKey = character == '\r'
                ? (ushort)0x0D
                : (ushort)0xE7; // VK_PACKET: inject Unicode without OEM-key ambiguity.
            records[index++] = new InputRecord
            {
                EventType = 1,
                KeyEvent = new KeyEventRecord
                {
                    KeyDown = true,
                    RepeatCount = 1,
                    VirtualKeyCode = virtualKey,
                    UnicodeChar = character
                }
            };
            records[index++] = new InputRecord
            {
                EventType = 1,
                KeyEvent = new KeyEventRecord
                {
                    KeyDown = false,
                    RepeatCount = 1,
                    VirtualKeyCode = virtualKey,
                    UnicodeChar = '\0'
                }
            };
        }

        if (!WriteConsoleInput(input, records, (uint)records.Length, out uint written) ||
            written != records.Length)
        {
            throw new InvalidOperationException(
                $"WriteConsoleInput failed: {Marshal.GetLastWin32Error()}");
        }
    }

    public static void SendBreak()
    {
        const uint CtrlBreakEvent = 1;
        if (!GenerateConsoleCtrlEvent(CtrlBreakEvent, 0))
        {
            throw new InvalidOperationException(
                $"GenerateConsoleCtrlEvent failed: {Marshal.GetLastWin32Error()}");
        }
    }
}
