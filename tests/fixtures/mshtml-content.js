// Runs in Windows Script Host, not in a web page. htmlfile is the document
// COM server under test; no embedded ActiveX control or JS-to-COM bridge.
var document = new ActiveXObject("htmlfile");
document.write("<!doctype html><title>initial</title><p>Windows host</p>");
document.writeln("<script>document.title='engine:'+navigator.userAgent.match(/Chrome\\/(\\d+)/)[1];</script>");
document.close();
if (document.title !== "engine:144") throw new Error("renderer script did not execute: " + document.title);
document.title = "Windows host \u4e16\u754c \uD83D\uDE80";
if (document.title !== "Windows host \u4e16\u754c \uD83D\uDE80") throw new Error("Unicode title mismatch");
var body = document.body;
if (body !== document.body) throw new Error("body identity changed");
if (body.parentElement !== document.documentElement) throw new Error("parent/root identity mismatch");
if (body.document !== document) throw new Error("owner document mismatch");
body.innerHTML = "<p id='node'>first</p>";
var node = document.getElementById("node");
if (!node || node !== document.getElementById("node")) throw new Error("element lookup identity mismatch");
node.innerText = "<b>literal</b> \u4e16\u754c";
if (node.innerHTML !== "&lt;b&gt;literal&lt;/b&gt; \u4e16\u754c") throw new Error("text was interpreted as markup");
node.id = "renamed";
if (document.getElementById("node") !== null || document.getElementById("renamed") !== node)
    throw new Error("live id lookup mismatch");
node.outerHTML = "<p id='replacement'>new</p>";
if (node.parentElement !== null || node.innerText !== "<b>literal</b> \u4e16\u754c")
    throw new Error("detached node silently rebound");
var created = document.createElement("article");
created.innerHTML = "<strong>created</strong>";
if (created.tagName !== "ARTICLE" || created.innerText !== "created" || created.parentElement !== null)
    throw new Error("created node mismatch");
WScript.Echo("WINDOWS SCRIPT HOST MSHTML ELEMENTS PASSED");
WScript.Echo("WINDOWS SCRIPT HOST MSHTML CONTENT PASSED");
body = node = created = null;
document = null;
CollectGarbage();
