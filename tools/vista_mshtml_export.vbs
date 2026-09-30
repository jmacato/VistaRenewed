' Fixed read-only binary extraction. Run with stock Windows Script Host, not IE.
Option Explicit
Dim files, names, index, fs, stream, request, source, destination
If WScript.Arguments.Count <> 1 Then WScript.Quit 2
files = Array("C:\Windows\SysWOW64\mshtml.dll", "C:\Windows\SysWOW64\ieframe.dll", "C:\Windows\SysWOW64\urlmon.dll", "C:\Windows\System32\mshtml.dll")
names = Array("mshtml-x86.dll", "ieframe-x86.dll", "urlmon-x86.dll", "mshtml-x64.dll")
Set fs = CreateObject("Scripting.FileSystemObject")
For index = 0 To UBound(files)
    source = files(index)
    destination = WScript.Arguments(0) & "/" & names(index)
    Set stream = CreateObject("ADODB.Stream")
    stream.Type = 1
    stream.Open
    stream.LoadFromFile source
    Set request = CreateObject("MSXML2.ServerXMLHTTP.6.0")
    request.setTimeouts 10000, 10000, 60000, 60000
    request.Open "POST", destination, False
    request.setRequestHeader "Content-Type", "application/octet-stream"
    request.setRequestHeader "X-Source-Path", source
    request.setRequestHeader "X-Source-Version", fs.GetFileVersion(source)
    request.Send stream.Read
    If request.Status <> 201 Then
        WScript.Echo "BINARY EXPORT FAILED status=" & request.Status
        WScript.Quit 1
    End If
    WScript.Echo source & " version=" & fs.GetFileVersion(source) & " bytes=" & stream.Size
    stream.Close
Next
WScript.Echo "VISTA NATIVE BINARIES EXPORTED"
