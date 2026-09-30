// Replacement data provider for Vista's original RSSFeeds gadget.  This keeps
// the shipped page, CSS, navigation, flyout, and settings page in place while
// bypassing the unavailable Feedstore COM server in the 64-bit Sidebar host.

var g_localRssRequestActive = false;
var g_localRssUrl = "http://10.0.2.2:8765/news?locale=" + encodeURIComponent(navigator.userLanguage || navigator.systemLanguage || "");
var g_localRssTitle = "Google News";

function localRssText(item, name) {
    var nodes = item.getElementsByTagName(name);
    if (!nodes || nodes.length == 0) {
        return "";
    }
    return nodes.item(0).text || "";
}

function localRssDate(value) {
    var date = new Date(value);
    if (isNaN(date.getTime())) {
        return "";
    }
    return date.toLocaleDateString();
}

function refreshRssFeedData() {
    var request;
    if (g_localRssRequestActive) {
        return;
    }
    g_localRssRequestActive = true;
    try {
        request = new ActiveXObject("MSXML2.XMLHTTP");
        request.onreadystatechange = function () {
            if (request.readyState != 4) {
                return;
            }
            g_localRssRequestActive = false;
            if (request.status != 200 || !request.responseXML || !request.responseXML.documentElement) {
                displayMessage(L_MS_ERRORMESSAGE, false);
                return;
            }
            try {
                var channel = request.responseXML.selectSingleNode("/rss/channel/title");
                g_localRssTitle = channel ? channel.text : "Google News";
                var nodes = request.responseXML.documentElement.getElementsByTagName("item");
                var feed = new makeFeed(g_localRssTitle, g_localRssUrl, nodes.length);
                for (var index = 0; index < nodes.length; index++) {
                    var title = removeNewLines(localRssText(nodes.item(index), "title"));
                    var link = localRssText(nodes.item(index), "link");
                    if (!title || !link) {
                        continue;
                    }
                    var itemModel = new feedItem(
                        title,
                        link,
                        false,
                        "local-news-" + index,
                        g_localRssTitle,
                        g_localRssTitle,
                        localRssDate(localRssText(nodes.item(index), "pubDate"))
                    );
                    itemModel.feedItemDescription = localRssText(nodes.item(index), "description");
                    feed.feedItems.push(itemModel);
                }
                if (feed.feedItems.length == 0) {
                    displayMessage(L_MS_ERRORMESSAGE, false);
                    return;
                }
                g_returnFeed = feed;
                g_currentArrayIndex = 0;
                g_timerFlag = true;
                setNextViewItems();
            } catch (error) {
                displayMessage(L_MS_ERRORMESSAGE, false);
            }
        };
        request.open("GET", g_localRssUrl, true);
        request.send();
    } catch (error) {
        g_localRssRequestActive = false;
        displayMessage(L_MS_ERRORMESSAGE, false);
    }
}

function loadData() {
    refreshRssFeedData();
    checkState();
    checkFlyforTimer();
    System.Gadget.onUndock = checkState;
    System.Gadget.onDock = checkState;
}

function createFeedDropDown() {
    AddFeedToDropDown(g_localRssTitle, g_localRssUrl);
    for (var index = 0; index < L_ARTICLES_TEXT.length; index++) {
        rssTotalsSelection.options[index] = new Option(L_ARTICLES_TEXT[index], articleArray[index]);
        rssTotalsSelection.options[index].title = L_ARTICLES_TEXT[index];
    }
}

function markAsRead() {
    if (g_returnFeed == null) {
        return;
    }
    for (var index = 0; index < g_returnFeed.feedItems.length; index++) {
        if (g_returnFeed.feedItems[index].feedItemUrl == g_feedURL) {
            g_returnFeed.feedItems[index].feedItemIsRead = true;
            g_viewElements.FeedItems[index % 4].className = "readItem";
        }
    }
}

function addContentToFlyout() {
    if (!System.Gadget.Flyout.show || g_returnFeed == null) {
        return;
    }
    try {
        var item = null;
        for (var index = 0; index < g_returnFeed.feedItems.length; index++) {
            if (g_returnFeed.feedItems[index].feedItemUrl == g_feedURL) {
                item = g_returnFeed.feedItems[index];
                break;
            }
        }
        if (item == null) {
            return;
        }
        var flyout = System.Gadget.Flyout.document;
        var title = flyout.getElementById("flyoutTitleLink");
        title.innerText = item.feedItemName;
        title.href = checkHref(item.feedItemUrl);
        title.title = item.feedItemName;
        var source = flyout.getElementById("flyoutPubDate");
        source.innerText = item.feedItemParent;
        source.href = g_localRssUrl;
        source.title = item.feedItemParent;
        // The relay has already reduced the feed description to safe formatting.
        flyout.getElementById("flyoutMain").innerHTML = item.feedItemDescription || item.feedItemName;
    } catch (error) {
    }
}
