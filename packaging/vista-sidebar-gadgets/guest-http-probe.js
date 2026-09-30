var urls = [
    { name: "NEWS", url: "http://10.0.2.2:8765/news", marker: "<item" },
    { name: "WEATHER", url: "http://10.0.2.2:8765/weather", marker: "location=\"Manila\"" },
    { name: "WEATHER SEARCH", url: "http://10.0.2.2:8765/weather-search?query=Tokyo", marker: "<location" },
    { name: "CURRENCY", url: "http://10.0.2.2:8765/currency", marker: "symbol=\"PHP\"" }
];

for (var i = 0; i < urls.length; i++) {
    var request = new ActiveXObject("MSXML2.XMLHTTP");
    try {
        request.open("GET", urls[i].url, false);
        request.send();
        if (request.status !== 200 || request.responseText.indexOf(urls[i].marker) < 0) {
            throw new Error("status=" + request.status + " marker=" + urls[i].marker);
        }
        WScript.Echo(urls[i].name + " OK " + request.responseText.length);
        if (urls[i].name == "NEWS") {
            var description = request.responseXML.selectSingleNode("//item/description").text;
            if (description.indexOf("<ol>") < 0 || description.indexOf("onclick=") >= 0 || description.indexOf("<script") >= 0) {
                throw new Error("RSS description was not safely formatted");
            }
            WScript.Echo("NEWS DESCRIPTION OK");
        }
    } catch (error) {
        WScript.Echo(urls[i].name + " ERROR " + error.number + " " + error.description);
    }
}

function checkCurrencyLabels() {
    var sourcePath = "C:\\Program Files\\Windows Sidebar\\Gadgets\\Currency.Gadget\\en-US\\js\\service.js";
    var source = new ActiveXObject("Scripting.FileSystemObject").OpenTextFile(sourcePath, 1, false, -1).ReadAll();
    var L_localizedStrings_Text = [];
    function getLocalizedString(key) {
        return L_localizedStrings_Text[key] === undefined ? null : L_localizedStrings_Text[key];
    }
    eval(source);
    var complete = false;
    var ok = false;
    var service = new CurrencyService();
    service.OnDataReady = function (data) {
        ok = data.RetCode == 200 && service.hsCurrencies["MRU"] && service.hsCurrencies["MRU"].Name == "MRU";
        complete = true;
    };
    service.GetCurrencies();
    for (var wait = 0; wait < 100 && !complete; wait++) {
        WScript.Sleep(100);
    }
    if (!ok) {
        throw new Error("currency labels unavailable");
    }
    WScript.Echo("CURRENCY LABELS OK");
}

try {
    checkCurrencyLabels();
} catch (error) {
    WScript.Echo("CURRENCY LABELS ERROR " + error.number + " " + error.description);
}
