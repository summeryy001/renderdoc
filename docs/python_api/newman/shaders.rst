API Reference: Shaders
======================

This is the API reference for the functions, classes, and enums in the ``newman`` module which represents the underlying interface that the UI is built on top of. For more high-level information and instructions on using the python API, see :doc:`../index`.

.. contents:: Sections
   :local:

.. currentmodule:: newman

Descriptors
-----------

.. autoclass:: newman.Descriptor
  :members:

.. autoclass:: newman.SamplerDescriptor
  :members:

.. autoclass:: newman.DescriptorFlags
  :members:
   
.. autoclass:: newman.DescriptorCategory
  :members:
   
.. autoclass:: newman.DescriptorType
  :members:
   
.. autofunction:: newman.CategoryForDescriptorType
.. autofunction:: newman.IsConstantBlockDescriptor
.. autofunction:: newman.IsReadOnlyDescriptor
.. autofunction:: newman.IsReadWriteDescriptor
.. autofunction:: newman.IsSamplerDescriptor

.. autoclass:: newman.DescriptorLogicalLocation
  :members:

.. autoclass:: newman.DescriptorRange
  :members:

.. autoclass:: newman.DescriptorAccess
  :members:

Reflection
----------

.. autoclass:: newman.ShaderReflection
  :members:

.. autoclass:: newman.ShaderStage
  :members:

.. autoclass:: newman.ShaderStageMask
  :members:

.. autofunction:: newman.MaskForStage
.. autofunction:: newman.FirstStageForMask

.. autoclass:: newman.SigParameter
  :members:

.. autoclass:: newman.ShaderBuiltin
  :members:

.. autoclass:: newman.ConstantBlock
  :members:

.. autoclass:: newman.ShaderSampler
  :members:

.. autoclass:: newman.ShaderResource
  :members:

Debug Info
----------

.. autoclass:: newman.ShaderDebugInfo
  :members:

.. autoclass:: newman.ShaderEncoding
  :members:
  
.. autoclass:: newman.KnownShaderTool
  :members:
  
.. autofunction:: newman.ToolExecutable
.. autofunction:: newman.ToolInput
.. autofunction:: newman.ToolOutput

.. autofunction:: newman.IsTextRepresentation

.. autoclass:: newman.ShaderEntryPoint
  :members:

.. autoclass:: newman.ShaderSourceFile
  :members:

.. autoclass:: newman.ShaderCompileFlags
  :members:

.. autoclass:: newman.ShaderCompileFlag
  :members:

.. autoclass:: newman.ShaderSourcePrefix
  :members:

Shader Constants
----------------

.. autoclass:: newman.ShaderConstant
  :members:

.. autoclass:: newman.ShaderConstantType
  :members:

.. autoclass:: newman.ShaderVariableFlags
  :members:

.. autoclass:: newman.VarType
  :members:

.. autofunction:: newman.VarTypeByteSize
.. autofunction:: newman.VarTypeCompType

Shader Debugging
----------------

.. autoclass:: newman.ShaderDebugTrace
  :members:

.. autoclass:: newman.ShaderDebugger
  :members:

.. autoclass:: newman.SourceVariableMapping
  :members:

.. autoclass:: newman.DebugVariableReference
  :members:

.. autoclass:: newman.DebugVariableType
  :members:

.. autoclass:: newman.LineColumnInfo
  :members:

.. autoclass:: newman.InstructionSourceInfo
  :members:

.. autoclass:: newman.ShaderDebugState
  :members:

.. autoclass:: newman.ShaderEvents
  :members:

.. autoclass:: newman.ShaderVariableChange
  :members:

Shader Variables
----------------
  
.. autoclass:: newman.ShaderVariable
  :members:

.. autoclass:: newman.ShaderValue
  :members:

.. autoclass:: newman.PointerVal
  :members:

.. autoclass:: newman.ShaderBindIndex
  :members:

.. autoclass:: newman.ShaderDirectAccess
  :members:
