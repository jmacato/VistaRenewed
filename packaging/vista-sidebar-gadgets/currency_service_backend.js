// Replacement data provider for Vista's original Currency gadget.
// Its page, CSS, controls, and currency.js UI remain the shipped versions.

function CurrencyService() {
    var me = this;
    var rates = new Array();

    this.hsCurrencies = new Array();
    this.IsAvailable = false;
    this.OnDataReady = null;

    function notify(data) {
        if (typeof me.OnDataReady == "function") {
            me.OnDataReady(data);
        }
    }

    function failed(status) {
        me.IsAvailable = false;
        notify({ RetCode: status || 0, Count: 0, Item: function () { return null; } });
    }

    this.Convert = function (amount, fromSymbol, toSymbol) {
        var input = (amount === "" ? 0 : parseFloat(amount));
        var fromRate = rates[fromSymbol];
        var toRate = rates[toSymbol];
        if (!isFinite(input) || !isFinite(fromRate) || !isFinite(toRate) || fromRate <= 0) {
            throw new Error("Currency rate unavailable");
        }
        var decimals = System.Gadget.docked ? 3 : 5;
        return ((input / fromRate) * toRate).toFixed(decimals);
    };

    this.GetCurrencies = function () {
        var request;
        try {
            request = new ActiveXObject("MSXML2.XMLHTTP");
            request.onreadystatechange = function () {
                if (request.readyState != 4) {
                    return;
                }
                if (request.status != 200 || !request.responseXML || !request.responseXML.documentElement) {
                    failed(request.status);
                    return;
                }
                try {
                    var nodes = request.responseXML.documentElement.getElementsByTagName("rate");
                    var records = new Array();
                    rates = new Array();
                    me.hsCurrencies = new Array();
                    for (var index = 0; index < nodes.length; index++) {
                        var symbol = nodes[index].getAttribute("symbol");
                        var value = parseFloat(nodes[index].getAttribute("value"));
                        if (!symbol || !isFinite(value) || value <= 0) {
                            continue;
                        }
                        if (getLocalizedString(symbol) === null) {
                            L_localizedStrings_Text[symbol] = symbol;
                        }
                        var name = getLocalizedString(symbol) || symbol;
                        var currency = {
                            Symbol: symbol,
                            Name: name,
                            NameForSorting: name,
                            PerDollar: value
                        };
                        rates[symbol] = value;
                        me.hsCurrencies[symbol] = currency;
                        records.push(currency);
                    }
                    if (records.length < 2) {
                        failed(502);
                        return;
                    }
                    me.IsAvailable = true;
                    notify({
                        RetCode: 200,
                        Count: records.length,
                        Item: function (itemIndex) { return records[itemIndex] || null; }
                    });
                } catch (error) {
                    failed(500);
                }
            };
            request.open("GET", "http://10.0.2.2:8765/currency", true);
            request.send();
        } catch (error) {
            failed(0);
        }
    };
}
