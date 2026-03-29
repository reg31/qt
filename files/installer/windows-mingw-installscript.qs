function Component() {}

Component.prototype.createOperations = function() {
    component.createOperations();
    var binPath = installer.value("TargetDir").replace(/\//g, "\\") + "\\qt-windows-mingw-release-dev\\bin";
    component.addOperation(
        "Execute",
        "powershell", "-Command",
        "[Environment]::SetEnvironmentVariable('PATH',[Environment]::GetEnvironmentVariable('PATH','User')+';'+'" + binPath + "','User')",
        "UNDOEXECUTE",
        "powershell", "-Command",
        "[Environment]::SetEnvironmentVariable('PATH',([Environment]::GetEnvironmentVariable('PATH','User').Split(';')|Where-Object{$_ -ne '" + binPath + "'})-join';','User')"
    );
}
