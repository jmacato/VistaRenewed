// Compatibility provider for Vista's original Weather gadget.  It presents
// Open-Meteo data in the old WLServices object shape so the shipped UI code,
// artwork, and settings behavior remain in use.

function LocalWeatherService() {
    var me = this;
    this.Celsius = true;
    this.RefreshInterval = 10;
    this.OnDataReady = null;

    function result(status, weather) {
        if (typeof me.OnDataReady == "function") {
            me.OnDataReady({
                RetCode: status,
                Count: weather ? 1 : 0,
                RequestPending: false,
                item: function (index) { return index == 0 ? weather : null; }
            });
        }
    }

    function degrees(value) {
        value = parseFloat(value);
        if (!isFinite(value)) {
            return 0;
        }
        return me.Celsius ? Math.round(value) : Math.round((value * 9 / 5) + 32);
    }

    this.SearchByCode = function (locationCode) {
        var request;
        try {
            request = new ActiveXObject("MSXML2.XMLHTTP");
            request.onreadystatechange = function () {
                if (request.readyState != 4) {
                    return;
                }
                if (request.status != 200 || !request.responseXML || !request.responseXML.documentElement) {
                    result(request.status || 0, null);
                    return;
                }
                try {
                    var root = request.responseXML.documentElement;
                    var forecastNodes = root.getElementsByTagName("forecast");
                    var forecasts = new Array();
                    for (var index = 0; index < forecastNodes.length; index++) {
                        forecasts.push({
                            Date: forecastNodes[index].getAttribute("date"),
                            Day: forecastNodes[index].getAttribute("day"),
                            High: degrees(forecastNodes[index].getAttribute("high")),
                            Low: degrees(forecastNodes[index].getAttribute("low")),
                            SkyCode: parseInt(forecastNodes[index].getAttribute("skyCode"), 10),
                            SkyText: forecastNodes[index].getAttribute("skyText")
                        });
                    }
                    if (forecasts.length < 4) {
                        result(502, null);
                        return;
                    }
                    var weather = {
                        Location: root.getAttribute("location"),
                        Latitude: parseFloat(root.getAttribute("latitude")),
                        Longitude: parseFloat(root.getAttribute("longitude")),
                        Temperature: degrees(root.getAttribute("temperature")),
                        SkyCode: parseInt(root.getAttribute("skyCode"), 10),
                        SkyText: root.getAttribute("skyText"),
                        Url: root.getAttribute("url"),
                        Attribution2: root.getAttribute("attribution"),
                        Forecast: function (forecastIndex) { return forecasts[forecastIndex] || forecasts[0]; }
                    };
                    weather.ForeCast = weather.Forecast;
                    result(200, weather);
                } catch (error) {
                    result(500, null);
                }
            };
            request.open("GET", "http://10.0.2.2:8765/weather?location=" + encodeURIComponent(locationCode || ""), true);
            request.send();
        } catch (error) {
            result(0, null);
        }
    };
}
