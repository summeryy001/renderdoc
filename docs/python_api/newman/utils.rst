API Reference: Utilities
========================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Maths
-----

.. autoclass:: FloatVector
  :members:

.. autofunction:: newman.HalfToFloat
.. autofunction:: newman.FloatToHalf

Logging & Versioning
--------------------

.. autofunction:: newman.LogMessage
.. autofunction:: newman.SetDebugLogFile
.. autofunction:: newman.GetLogFile
.. autofunction:: newman.GetCurrentProcessMemoryUsage
.. autofunction:: newman.DumpObject

.. autoclass:: LogType
  :members:


Versioning
----------

.. autofunction:: newman.GetVersionString
.. autofunction:: newman.GetCommitHash
.. autofunction:: newman.IsReleaseBuild

Settings
--------

.. autofunction:: newman.GetConfigSetting
.. autofunction:: newman.SetConfigSetting
.. autofunction:: newman.SaveConfigSettings

Self-hosted captures
--------------------

.. autofunction:: newman.CanSelfHostedCapture
.. autofunction:: newman.StartSelfHostCapture
.. autofunction:: newman.EndSelfHostCapture
