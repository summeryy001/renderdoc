API Reference: Capturing
========================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Execution & Injection
---------------------

.. autofunction:: newman.ExecuteAndInject
.. autofunction:: newman.InjectIntoProcess

.. autoclass:: newman.CaptureOptions
  :members:
  
.. autoclass:: newman.EnvironmentModification
  :members:

.. autoclass:: newman.EnvMod
  :members:

.. autoclass:: newman.EnvSep
  :members:

.. autoclass:: newman.ExecuteResult
  :members:

Global Hooking
--------------

.. autofunction:: newman.StartGlobalHook
.. autofunction:: newman.StopGlobalHook
.. autofunction:: newman.IsGlobalHookActive
.. autofunction:: newman.CanGlobalHook

Target Control
--------------

.. autofunction:: newman.EnumerateRemoteTargets
.. autofunction:: newman.CreateTargetControl

.. autoclass:: newman.TargetControl
  :members:

.. autoclass:: newman.TargetControlMessage
  :members:

.. autoclass:: newman.TargetControlMessageType
  :members:

.. autoclass:: newman.NewCaptureData
  :members:

.. autoclass:: newman.APIUseData
  :members:

.. autoclass:: newman.BusyData
  :members:

.. autoclass:: newman.NewChildData
  :members:

