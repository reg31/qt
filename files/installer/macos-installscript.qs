function Component() {}

Component.prototype.createOperations = function() {
    component.createOperations();
    var targetDir = installer.value("TargetDir");
    component.addOperation("Execute", "xattr", "-cr", targetDir);
    component.addOperation("Execute", "find", targetDir, "-type", "f", "-exec", "chmod", "+x", "{}", "+");
    var binPath = targetDir + "/qt-macos-release-dev/bin";
    component.addOperation("AppendFile", installer.value("HomeDir") + "/.zprofile", "\nexport PATH=\"" + binPath + ":$PATH\"\n");
}
