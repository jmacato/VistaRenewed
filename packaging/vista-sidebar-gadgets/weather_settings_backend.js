// Compatibility location-search provider for Vista's original Weather
// settings page.  The page and its controls stay untouched.

function LocalWeatherLookupService() {
    var me = this;
    this.OnDataReady = null;

    function respond(status, locations) {
        if (typeof me.OnDataReady == "function") {
            me.OnDataReady({
                RetCode: status,
                Count: locations.length,
                count: locations.length,
                item: function (index) { return locations[index] || null; }
            });
        }
    }

    this.SearchByLocation = function (query) {
        var request;
        try {
            request = new ActiveXObject("MSXML2.XMLHTTP");
            request.onreadystatechange = function () {
                if (request.readyState != 4) {
                    return;
                }
                if (request.status != 200 || !request.responseXML || !request.responseXML.documentElement) {
                    respond(request.status || 0, new Array());
                    return;
                }
                try {
                    var nodes = request.responseXML.documentElement.getElementsByTagName("location");
                    var locations = new Array();
                    for (var index = 0; index < nodes.length; index++) {
                        var name = nodes[index].getAttribute("name");
                        var fullname = nodes[index].getAttribute("fullname") || name;
                        locations.push({
                            Location: name,
                            Fullname: fullname,
                            LocationCode: nodes[index].getAttribute("code"),
                            ZipCode: "",
                            SearchDistance: 0,
                            SearchScore: 1,
                            SearchLocation: fullname
                        });
                    }
                    respond(200, locations);
                } catch (error) {
                    respond(500, new Array());
                }
            };
            request.open("GET", "http://10.0.2.2:8765/weather-search?query=" + encodeURIComponent(query), true);
            request.send();
        } catch (error) {
            respond(0, new Array());
        }
    };
}
