API Reference: Replay Control
=============================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Initialisation and Shutdown
---------------------------

.. autofunction:: newman.InitialiseReplay
.. autofunction:: newman.ShutdownReplay

.. autoclass:: newman.GlobalEnvironment
  :members:

.. autoclass:: newman.ResultCode
  :members:

.. autoclass:: newman.ResultDetails
  :members:

Capture File Access
-------------------

.. autofunction:: newman.OpenCaptureFile

.. autoclass:: newman.CaptureAccess
  :members:

.. autoclass:: newman.CaptureFile
  :members:

.. autoclass:: newman.ReplaySupport
  :members:

.. autoclass:: newman.CaptureFileFormat
  :members:

.. autoclass:: newman.SectionProperties
  :members:

.. autoclass:: newman.SectionType
  :members:

.. autoclass:: newman.SectionFlags
  :members:

.. autoclass:: newman.Thumbnail
  :members:

GPU Enumeration
---------------

.. autoclass:: newman.GPUDevice
  :members:

.. autoclass:: newman.GPUVendor
  :members:

.. autofunction:: newman.GPUVendorFromPCIVendor

.. autoclass:: newman.GraphicsAPI
  :members:

.. autofunction:: newman.IsD3D

.. autofunction:: newman.GetDriverInformation

.. autoclass:: newman.DriverInformation
  :members:

Replay Controller
-----------------

.. autoclass:: newman.ReplayController
  :members:

.. autoclass:: newman.ReplayOptions
  :members:

.. autoclass:: newman.ReplayOptimisationLevel
  :members:

.. autoclass:: newman.APIProperties
  :members:

Device Protocols
----------------

.. autoclass:: newman.DeviceProtocolController
  :members:

.. autofunction:: newman.GetSupportedDeviceProtocols
.. autofunction:: newman.GetDeviceProtocolController

Remote Servers
--------------

.. autoclass:: newman.RemoteServer
  :members:

.. autofunction:: newman.CreateRemoteServerConnection
.. autofunction:: newman.CheckRemoteServerConnection
.. autofunction:: newman.BecomeRemoteServer

.. autoclass:: newman.PathEntry
  :members:

.. autoclass:: newman.PathProperty
  :members:
