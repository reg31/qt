function Component() {}

Component.prototype.createOperations = function() {
    component.createOperations();
    var targetDir = installer.value("TargetDir");
    component.addOperation("Execute", "xattr", "-cr", targetDir, "UNDOEXECUTE", "");
    component.addOperation("Execute", "find", targetDir, "-type", "f", "-exec", "chmod", "+x", "{}", "+", "UNDOEXECUTE", "");
}
