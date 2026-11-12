API Reference: Structured Data
==============================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Type information
----------------

.. autoclass:: SDType
  :members:

.. autoclass:: SDBasic
  :members:

.. autoclass:: SDTypeFlags
  :members:

Objects
-------

.. autoclass:: SDObject
  :members:

.. autoclass:: SDObjectData
  :members:

.. autoclass:: SDObjectPODData
  :members:

Chunks
------

.. autoclass:: SDChunk
  :members:

.. autoclass:: SDChunkMetaData
  :members:

.. autoclass:: SDChunkFlags
  :members:

Structured File
---------------

.. autoclass:: SDFile
  :members:

Creation Helper Functions
-------------------------

.. autofunction:: newman.makeSDArray
.. autofunction:: newman.makeSDBool
.. autofunction:: newman.makeSDEnum
.. autofunction:: newman.makeSDFloat
.. autofunction:: newman.makeSDInt32
.. autofunction:: newman.makeSDInt64
.. autofunction:: newman.makeSDResourceId
.. autofunction:: newman.makeSDString
.. autofunction:: newman.makeSDStruct
.. autofunction:: newman.makeSDUInt32
.. autofunction:: newman.makeSDUInt64
