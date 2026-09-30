// Add the maintained news feed to the current user's Vista Common Feed List.
// The RSSFeeds gadget consumes this store directly, so its shipped page and
// script stay untouched.

var locale = new ActiveXObject("WScript.Shell").RegRead("HKCU\\Control Panel\\International\\LocaleName");
var FEED_URL = "http://10.0.2.2:8765/news?locale=" + encodeURIComponent(locale);
var FEED_NAME = "Google News (" + locale + ")";

function main() {
    var manager = new ActiveXObject("Microsoft.FeedsManager");
    var feed;
    if (manager.IsSubscribed(FEED_URL)) {
        feed = manager.GetFeedByUrl(FEED_URL);
    } else {
        feed = manager.RootFolder.CreateFeed(FEED_NAME, FEED_URL);
    }
    if (feed.Name != FEED_NAME) {
        feed.Rename(FEED_NAME);
    }
    feed.Download();
    WScript.Echo("RSS FEED READY " + feed.ItemCount);
}

try {
    main();
} catch (error) {
    WScript.Echo("RSS FEED ERROR " + (error.description || error.message || error));
    WScript.Quit(1);
}
