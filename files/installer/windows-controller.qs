function Controller() {
    var qtRoot = detectQtRoot();
    if (qtRoot !== "") {
        installer.setValue("TargetDir", qtRoot + "/dev");
    }
}

function detectQtRoot() {
    var regKey = installer.registryValue("HKEY_LOCAL_MACHINE\\SOFTWARE\\Qt\\", "Install_Dir");
    if (regKey !== "") return regKey.replace(/\\/g, "/");
    var candidates = ["C:/Qt", "D:/Qt"];
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
