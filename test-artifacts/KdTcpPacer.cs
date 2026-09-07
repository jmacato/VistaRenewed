using System.Net;
using System.Net.Sockets;

const int listenPort = 2021;
const int qemuPort = 2020;
// QEMU's emulated 16550 FIFO and checked Vista's early polled KD receiver can
// lose a coalesced TCP burst while the guest runs under single-core TCG.  An
// eight-byte quantum stays below both the 16-byte FIFO and the approximately
// 11.5-byte/ms drain rate at 115200 baud.  A smaller per-byte delay makes a
// complete MANIP packet exceed Vista's KD packet timeout.
const int writeQuantum = 8;
const int writeDelayMilliseconds = 1;
const string breakTriggerPath = "/tmp/vista-kd-break.trigger";

var listener = new TcpListener(IPAddress.Any, listenPort);
listener.Start();
Console.WriteLine($"KD TCP pacer listening on 0.0.0.0:{listenPort}");

while (true)
{
    try
    {
        Console.WriteLine("waiting for KD");
        using TcpClient kd = await listener.AcceptTcpClientAsync();
        kd.NoDelay = true;
        using NetworkStream kdStream = kd.GetStream();

        // com:ipport starts every session with a 16-byte KD RESET control
        // packet.  Read it before opening QEMU's serial socket so a target
        // DEBUG_IO packet can never race ahead of the debugger's reset.
        byte[] initialReset = await ReadExactly(kdStream, 16);
        Console.WriteLine("KD connected; captured initial reset " +
            Convert.ToHexString(initialReset));

        // Connect to QEMU only after KD is present.  Forward every target byte
        // unchanged: attempting to drain or align the stream here discarded
        // Vista's first complete state-change/debug-I/O packet and made the
        // debugger wait forever even though the target was transmitting.
        Console.WriteLine($"KD connected; connecting to QEMU on 127.0.0.1:{qemuPort}");
        TcpClient? qemu = null;
        while (qemu == null)
        {
            var candidate = new TcpClient { NoDelay = true };
            try
            {
                await candidate.ConnectAsync(IPAddress.Loopback, qemuPort);
                qemu = candidate;
            }
            catch (SocketException error)
            {
                candidate.Dispose();
                Console.WriteLine($"QEMU not ready ({error.SocketErrorCode}); retrying");
                await Task.Delay(250);
            }
        }
        using TcpClient connectedQemu = qemu;
        using NetworkStream qemuStream = connectedQemu.GetStream();
        await qemuStream.WriteAsync(initialReset);
        await qemuStream.FlushAsync();
        Console.WriteLine("QEMU connected; forwarded reset first; starting transparent pumps");

        Task fromQemu = Pump(qemuStream, kdStream, "QEMU->KD",
            paced: false, alignToKdPacket: false);
        Task fromKd = Pump(kdStream, qemuStream, "KD->QEMU",
            paced: true, alignToKdPacket: false);
        // Do not inject a break during the initial Vista boot handshake. The
        // debug boot already emits a state-change packet, and an overlapping
        // break can make modern KD request a MANIP reply while another state
        // packet is queued. Use breakTriggerPath only after the target runs.
        Task breakInjector = InjectBreaks(qemuStream, breakTriggerPath);
        await Task.WhenAny(fromQemu, fromKd, breakInjector);
        Console.WriteLine("one direction ended; reconnecting both ends");
    }
    catch (Exception error)
    {
        Console.WriteLine($"pacer error: {error.GetType().Name}: {error.Message}");
    }

    await Task.Delay(500);
}

static async Task<byte[]> ReadExactly(NetworkStream stream, int length)
{
    byte[] buffer = new byte[length];
    int offset = 0;
    while (offset < length)
    {
        int read = await stream.ReadAsync(buffer.AsMemory(offset, length - offset));
        if (read == 0)
            throw new EndOfStreamException("KD disconnected before its initial reset");
        offset += read;
    }
    return buffer;
}

static async Task InjectBreaks(NetworkStream qemuStream, string triggerPath)
{
    while (true)
    {
        if (File.Exists(triggerPath))
        {
            File.Delete(triggerPath);
            // The transport recognizes an out-of-band ASCII 'b' as a break-in.
            // Only create the trigger while the target is running and KD traffic
            // is idle, so it cannot split a normal debugger packet.
            await qemuStream.WriteAsync(new byte[] { 0x62 });
            await qemuStream.FlushAsync();
            Console.WriteLine("injected requested KD break-in byte 0x62");
        }

        await Task.Delay(100);
    }
}

static async Task Pump(
    NetworkStream source, NetworkStream destination,
    string direction, bool paced, bool alignToKdPacket)
{
    var buffer = new byte[65536];
    long total = 0;
    int previewRemaining = 512;
    bool aligned = !alignToKdPacket;
    byte leaderByte = 0;
    int leaderRun = 0;
    while (true)
    {
        int read = await source.ReadAsync(buffer);
        if (read == 0)
            break;

        if (previewRemaining > 0)
        {
            int previewLength = Math.Min(read, previewRemaining);
            Console.WriteLine($"{direction} hex: " +
                Convert.ToHexString(buffer, 0, previewLength));
            previewRemaining -= previewLength;
        }

        int offset = 0;
        int count = read;
        if (!aligned)
        {
            for (int index = 0; index < read; index++)
            {
                byte value = buffer[index];
                // KD packets use either a 0x69696969 control leader or a
                // 0x30303030 data/state leader. Preattach bytes were drained
                // above, so the first complete leader here belongs to the new
                // debugger session (including Vista's early-boot state packet).
                if (value == 0x30 || value == 0x69)
                {
                    if (leaderRun == 0 || value != leaderByte)
                    {
                        leaderByte = value;
                        leaderRun = 1;
                    }
                    else
                    {
                        leaderRun++;
                    }

                    if (leaderRun == 4)
                    {
                        var leader = new[] { value, value, value, value };
                        await destination.WriteAsync(leader);
                        await destination.FlushAsync();
                        total += leader.Length;
                        offset = index + 1;
                        count = read - offset;
                        aligned = true;
                        Console.WriteLine($"{direction}: aligned on " +
                            $"{Convert.ToHexString(leader)} after discarding stream prefix");
                        break;
                    }
                }
                else
                {
                    leaderRun = 0;
                }
            }

            if (!aligned)
                continue;
        }

        if (count == 0)
            continue;

        if (!paced)
        {
            await destination.WriteAsync(buffer.AsMemory(offset, count));
            await destination.FlushAsync();
        }
        else
        {
            for (int pacedOffset = offset;
                 pacedOffset < offset + count;
                 pacedOffset += writeQuantum)
            {
                int quantum = Math.Min(writeQuantum, offset + count - pacedOffset);
                await destination.WriteAsync(buffer.AsMemory(pacedOffset, quantum));
                await destination.FlushAsync();
                await Task.Delay(writeDelayMilliseconds);
            }
        }

        total += count;
        if (total <= 64 || total % 4096 < count)
            Console.WriteLine($"{direction}: {total} bytes");
    }

    Console.WriteLine($"{direction} ended after {total} bytes");
}
