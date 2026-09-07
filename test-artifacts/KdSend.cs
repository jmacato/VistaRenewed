using System.Runtime.InteropServices;

if (args.Length != 2 || !uint.TryParse(args[0], out uint kdPid))
{
    Console.Error.WriteLine("usage: dotnet run KdSend.cs <kd-pid> <command|--break>");
    return 2;
}

Native.FreeConsole();
if (!Native.AttachConsole(kdPid))
{
    Console.Error.WriteLine($"AttachConsole failed: {Marshal.GetLastWin32Error()}");
    return 1;
}
if (!Native.SetConsoleCtrlHandler(IntPtr.Zero, true))
{
    Console.Error.WriteLine($"SetConsoleCtrlHandler failed: {Marshal.GetLastWin32Error()}");
    return 1;
}

IntPtr input = Native.CreateFile(
    "CONIN$", Native.GenericRead | Native.GenericWrite,
    Native.FileShareRead | Native.FileShareWrite, IntPtr.Zero,
    Native.OpenExisting, 0, IntPtr.Zero);
if (input == IntPtr.Zero || input == new IntPtr(-1))
{
    Console.Error.WriteLine($"CreateFile(CONIN$) failed: {Marshal.GetLastWin32Error()}");
    return 1;
}

if (args[1] == "--break")
    Native.SendBreak();
else
    Native.SendConsoleInput(input, args[1] + "\r");
return 0;

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
    public static extern bool AttachConsole(uint processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);

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

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);

    public static void SendBreak()
    {
        const uint CtrlBreakEvent = 1;
        if (!GenerateConsoleCtrlEvent(CtrlBreakEvent, 0))
            throw new InvalidOperationException(
                $"GenerateConsoleCtrlEvent failed: {Marshal.GetLastWin32Error()}");
    }

    public static void SendConsoleInput(IntPtr input, string text)
    {
        var records = new InputRecord[text.Length * 2];
        int index = 0;
        foreach (char character in text)
        {
            ushort virtualKey = character == '\r' ? (ushort)0x0D : (ushort)0xE7;
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
                $"WriteConsoleInput wrote {written}/{records.Length}: {Marshal.GetLastWin32Error()}");
        }
    }

}
