function Controller() {
    var qtRoot = detectQtRoot();
    if (qtRoot !== "") {
        installer.setValue("TargetDir", qtRoot + "/dev");
    }
}

function detectQtRoot() {
    var candidates = [
        installer.value("HomeDir") + "/Qt",
        "/usr/local/Qt",
        "/opt/Qt"
    ];
    for (var i = 0; i < candidates.length; i++) {
        if (installer.fileExists(candidates[i])) return candidates[i];
    }
    return "";
}

Controller.prototype.TargetDirectoryPageCallback = function() {
    var page = gui.pageWidgetByObjectName("TargetDirectoryPage");
    if (page) {
        page.MessageLabel.setText("Select your Qt installation root. The kits will be installed into the 'dev' subfolder.");
    }
}
