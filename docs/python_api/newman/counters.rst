API Reference: Performance Counters
===================================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Counters
--------

.. autoclass:: newman.CounterDescription
  :members:

.. autoclass:: newman.CounterUnit
  :members:

.. autoclass:: newman.Uuid
  :members:

Counter Types
-------------

.. autoclass:: newman.GPUCounter
  :members:

.. autofunction:: newman.IsAMDCounter
.. autofunction:: newman.IsARMCounter
.. autofunction:: newman.IsGenericCounter
.. autofunction:: newman.IsIntelCounter
.. autofunction:: newman.IsNvidiaCounter
.. autofunction:: newman.IsVulkanExtendedCounter

Results
-------

.. autoclass:: newman.CounterResult
  :members:

.. autoclass:: newman.CounterValue
  :members:
