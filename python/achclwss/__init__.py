"""Drive the ACHCLWSS suite from Python.

    import achclwss

    with achclwss.Suite() as suite:
        img = suite.photo.open(path="cat.jpg").doc
        suite.photo.resize(doc=img, width=800)
        suite.photo.save(doc=img, path="cat_small.jpg")

    achclwss.run_batch("convert.file input=a.png output=a.jpg")
"""

from .client import (
    AchclwssError,
    BatchResult,
    CommandError,
    Result,
    Suite,
    find_binary,
    run_batch,
)

__version__ = "0.5.0"

__all__ = [
    "AchclwssError",
    "BatchResult",
    "CommandError",
    "Result",
    "Suite",
    "find_binary",
    "run_batch",
    "__version__",
]
