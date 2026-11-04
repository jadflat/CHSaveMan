"""tools/chsim as a package (for the editable install). The tools themselves
import the flat modules with tools/chsim on sys.path: `from chsim import build`."""
from .chsim import build, find_cxx  # noqa: F401
