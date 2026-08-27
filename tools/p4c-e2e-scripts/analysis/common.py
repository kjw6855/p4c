import os

HOME = os.path.expanduser("~")
def home_to_tilde(path: str) -> str:
    path = str(path)
    if path == str(HOME):
        return "~"
    if path.startswith(str(HOME) + "/"):
        return "~" + path[len(str(HOME)):]
    return path

