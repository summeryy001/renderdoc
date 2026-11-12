API Reference: Common Pipeline State Abstraction
================================================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

.. autoclass:: PipeState
  :members:

General
-------

.. autoclass:: newman.Offset
  :members:

Vertex Inputs
-------------

.. autoclass:: newman.BoundVBuffer
  :members:

.. autoclass:: newman.VertexInputAttribute
  :members:

.. autoclass:: newman.Topology
  :members:
  
.. autofunction:: newman.NumVerticesPerPrimitive
.. autofunction:: newman.VertexOffset
.. autofunction:: newman.PatchList_Count
.. autofunction:: newman.PatchList_Topology
.. autofunction:: newman.IsStrip

Shader Resource Bindings
------------------------

.. autoclass:: newman.UsedDescriptor
  :members:

.. autoclass:: newman.BindType
  :members:

.. autoclass:: newman.TextureSwizzle
  :members:

.. autoclass:: newman.TextureSwizzle4
  :members:

Samplers
--------

.. autoclass:: newman.AddressMode
  :members:

.. autoclass:: newman.TextureFilter
  :members:

.. autoclass:: newman.FilterMode
  :members:
  
.. autoclass:: newman.FilterFunction
  :members:

.. autoclass:: newman.ChromaSampleLocation
  :members:

.. autoclass:: newman.YcbcrConversion
  :members:

.. autoclass:: newman.YcbcrRange
  :members:

Viewport and Scissor
--------------------

.. autoclass:: newman.Viewport
  :members:

.. autoclass:: newman.Scissor
  :members:

Rasterizer
----------

.. autoclass:: newman.CullMode
  :members:

.. autoclass:: newman.DepthBiasMode
  :members:

.. autoclass:: newman.FillMode
  :members:

.. autoclass:: newman.ConservativeRaster
  :members:

.. autoclass:: newman.LineRaster
  :members:

.. autoclass:: newman.ShadingRateCombiner
  :members:

.. autoclass:: newman.RasterState
  :members:


Depth and Stencil
-----------------

.. autoclass:: newman.DepthTestState
  :members:

.. autoclass:: newman.StencilFace
  :members:

.. autoclass:: newman.StencilOperation
  :members:

.. autoclass:: newman.CompareFunction
  :members:

Blending
--------

.. autoclass:: newman.ColorBlend
  :members:

.. autoclass:: newman.BlendEquation
  :members:

.. autoclass:: newman.BlendMultiplier
  :members:

.. autoclass:: newman.BlendOperation
  :members:

.. autoclass:: newman.LogicOperation
  :members:

Shader Messages
---------------

.. autoclass:: newman.ShaderMessage
  :members:

.. autoclass:: newman.ShaderMessageLocation
  :members:

.. autoclass:: newman.ShaderMeshMessageLocation
  :members:

.. autoclass:: newman.ShaderVertexMessageLocation
  :members:

.. autoclass:: newman.ShaderPixelMessageLocation
  :members:

.. autoclass:: newman.ShaderGeometryMessageLocation
  :members:

.. autoclass:: newman.ShaderComputeMessageLocation
  :members:
