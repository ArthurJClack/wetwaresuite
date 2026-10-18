"""Drive the wetwaresuite suite from Python.

    import wetwaresuite

    with wetwaresuite.Suite() as suite:
        img = suite.photo.open(path="cat.jpg").doc
        suite.photo.resize(doc=img, width=800)
        suite.photo.save(doc=img, path="cat_small.jpg")

    wetwaresuite.run_batch("convert.file input=a.png output=a.jpg")
"""

from .client import (
    WetwaresuiteError,
    BatchResult,
    CommandError,
    Result,
    Suite,
    find_binary,
    run_batch,
)

__version__ = "0.6.0"

__all__ = [
    "WetwaresuiteError",
    "BatchResult",
    "CommandError",
    "Result",
    "Suite",
    "find_binary",
    "run_batch",
    "__version__",
]
