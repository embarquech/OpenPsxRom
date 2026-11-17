# Sphinx configuration for the OpenPsxRom documentation.
#   python -m sphinx -b html docs docs/_build/html
project = 'OpenPsxRom'
author = 'embarquech'
copyright = '2026, embarquech (GPL-3.0-or-later)'
release = '0.6'

extensions = []
exclude_patterns = ['_build']
language = 'en'

try:
    import sphinx_rtd_theme  # noqa: F401
    html_theme = 'sphinx_rtd_theme'
except ImportError:
    html_theme = 'alabaster'
html_title = 'OpenPsxRom documentation'
html_static_path = []
