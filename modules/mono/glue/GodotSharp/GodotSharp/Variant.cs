using System;
using System.Diagnostics.CodeAnalysis;
using System.Runtime.CompilerServices;
using Godot.NativeInterop;

namespace Godot;

#nullable enable

[SuppressMessage("ReSharper", "RedundantNameQualifier")]
public partial struct Variant : IDisposable
{
    internal godot_variant.movable NativeVar;
    private object? _obj;
    private Disposer? _disposer;

    private class Disposer : IDisposable
    {
        private godot_variant.movable _native;

        private WeakReference<IDisposable> _weakReferenceToSelf;

        public Disposer(in godot_variant.movable nativeVar)
        {
            _native = nativeVar;
            _weakReferenceToSelf = DisposablesTracker.RegisterDisposable(this);
        }

        public void Dispose()
        {
            _native.DangerousSelfRef.Dispose();
            DisposablesTracker.UnregisterDisposable(_weakReferenceToSelf);
        }
    }

    private Variant(in godot_variant nativeVar)
    {
        NativeVar = (godot_variant.movable)nativeVar;
        _obj = null;

        switch (nativeVar.Type)
        {
            case Type.Nil:
            case Type.Bool:
            case Type.Int:
            case Type.Float:
            case Type.Vector2:
            case Type.Vector2i:
            case Type.Rect2:
            case Type.Rect2i:
            case Type.Vector3:
            case Type.Vector3i:
            case Type.Plane:
            case Type.Quaternion:
            case Type.Color:
            case Type.Rid:
                _disposer = null;
                break;
            default:
            {
                _disposer = new Disposer(NativeVar);
                break;
            }
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    // Explicit name to make it very clear
    public static Variant CreateTakingOwnershipOfDisposableValue(in godot_variant nativeValueToOwn) =>
        new(nativeValueToOwn);

    // Explicit name to make it very clear
    public static Variant CreateCopyingBorrowed(in godot_variant nativeValueToOwn) =>
        new(NativeFuncs.godotsharp_variant_new_copy(nativeValueToOwn));

    /// <summary>
    /// Constructs a new <see cref="Godot.NativeInterop.godot_variant"/> from this instance.
    /// The caller is responsible of disposing the new instance to avoid memory leaks.
    /// </summary>
    public godot_variant CopyNativeVariant() =>
        NativeFuncs.godotsharp_variant_new_copy((godot_variant)NativeVar);

    public void Dispose()
    {
        _disposer?.Dispose();
        NativeVar = default;
        _obj = null;
    }

    // TODO: Consider renaming Variant.Type to VariantType and this property to Type. VariantType would also avoid ambiguity with System.Type.
    public Type VariantType => NativeVar.DangerousSelfRef.Type;

    public object? Obj
    {
        get
        {
            if (_obj == null)
                _obj = Marshaling.ConvertVariantToManagedObject((godot_variant)NativeVar);

            return _obj;
        }
    }

    // TODO: Consider implicit operators

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool AsBool() =>
        VariantUtils.ConvertToBool((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public char AsChar() =>
        (char)VariantUtils.ConvertToUInt16((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public sbyte AsSByte() =>
        VariantUtils.ConvertToInt8((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public short AsInt16() =>
        VariantUtils.ConvertToInt16((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public int AsInt32() =>
        VariantUtils.ConvertToInt32((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public long AsInt64() =>
        VariantUtils.ConvertToInt64((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public byte AsByte() =>
        VariantUtils.ConvertToUInt8((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ushort AsUInt16() =>
        VariantUtils.ConvertToUInt16((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public uint AsUInt32() =>
        VariantUtils.ConvertToUInt32((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public ulong AsUInt64() =>
        VariantUtils.ConvertToUInt64((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public float AsSingle() =>
        VariantUtils.ConvertToFloat32((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public double AsDouble() =>
        VariantUtils.ConvertToFloat64((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public string AsString() =>
        VariantUtils.ConvertToStringObject((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector2 AsVector2() =>
        VariantUtils.ConvertToVector2((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector2i AsVector2i() =>
        VariantUtils.ConvertToVector2i((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Rect2 AsRect2() =>
        VariantUtils.ConvertToRect2((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Rect2i AsRect2i() =>
        VariantUtils.ConvertToRect2i((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Transform2D AsTransform2D() =>
        VariantUtils.ConvertToTransform2D((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector3 AsVector3() =>
        VariantUtils.ConvertToVector3((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector3i AsVector3i() =>
        VariantUtils.ConvertToVector3i((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Basis AsBasis() =>
        VariantUtils.ConvertToBasis((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Quaternion AsQuaternion() =>
        VariantUtils.ConvertToQuaternion((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Transform3D AsTransform3D() =>
        VariantUtils.ConvertToTransform3D((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public AABB AsAABB() =>
        VariantUtils.ConvertToAABB((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Color AsColor() =>
        VariantUtils.ConvertToColor((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Plane AsPlane() =>
        VariantUtils.ConvertToPlane((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Callable AsCallable() =>
        VariantUtils.ConvertToCallableManaged((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SignalInfo AsSignalInfo() =>
        VariantUtils.ConvertToSignalInfo((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public byte[] AsByteArray() =>
        VariantUtils.ConvertAsPackedByteArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public int[] AsInt32Array() =>
        VariantUtils.ConvertAsPackedInt32ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public long[] AsInt64Array() =>
        VariantUtils.ConvertAsPackedInt64ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public float[] AsFloat32Array() =>
        VariantUtils.ConvertAsPackedFloat32ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public double[] AsFloat64Array() =>
        VariantUtils.ConvertAsPackedFloat64ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public string[] AsStringArray() =>
        VariantUtils.ConvertAsPackedStringArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector2[] AsVector2Array() =>
        VariantUtils.ConvertAsPackedVector2ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Vector3[] AsVector3Array() =>
        VariantUtils.ConvertAsPackedVector3ArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Color[] AsColorArray() =>
        VariantUtils.ConvertAsPackedColorArrayToSystemArray((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public T[] AsGodotObjectArray<T>()
        where T : Godot.Object =>
        VariantUtils.ConvertToSystemArrayOfGodotObject<T>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public T[] AsSystemArrayOfSupportedType<T>() =>
        VariantUtils.ConvertToSystemArrayOfSupportedType<T>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Collections.Dictionary<TKey, TValue> AsGodotGenericDictionary<TKey, TValue>() =>
        VariantUtils.ConvertToGenericDictionaryObject<TKey, TValue>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Collections.Array<T> AsGodotGenericArray<T>() =>
        VariantUtils.ConvertToGenericArrayObject<T>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public System.Collections.Generic.Dictionary<TKey, TValue> AsSystemGenericDictionary<TKey, TValue>()
        where TKey : notnull =>
        VariantUtils.ConvertToSystemGenericDictionary<TKey, TValue>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public System.Collections.Generic.List<T> AsSystemGenericList<T>() =>
        VariantUtils.ConvertToSystemGenericList<T>((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Godot.Object AsGodotObject() =>
        VariantUtils.ConvertToGodotObject((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public StringName AsStringName() =>
        VariantUtils.ConvertToStringNameObject((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public NodePath AsNodePath() =>
        VariantUtils.ConvertToNodePathObject((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RID AsRID() =>
        VariantUtils.ConvertToRID((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Collections.Dictionary AsGodotDictionary() =>
        VariantUtils.ConvertToDictionaryObject((godot_variant)NativeVar);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Collections.Array AsGodotArray() =>
        VariantUtils.ConvertToArrayObject((godot_variant)NativeVar);


    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(bool from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromBool(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(char from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(sbyte from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(short from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(int from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(long from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(byte from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(ushort from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(uint from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(ulong from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromInt(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(float from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromFloat(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(double from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromFloat(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(string from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromString(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Vector2 from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromVector2(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Vector2i from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromVector2i(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Rect2 from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromRect2(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Rect2i from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromRect2i(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Transform2D from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromTransform2D(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Vector3 from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromVector3(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Vector3i from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromVector3i(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Basis from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromBasis(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Quaternion from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromQuaternion(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Transform3D from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromTransform3D(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(AABB from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromAABB(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Color from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromColor(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Plane from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPlane(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Callable from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromCallable(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(SignalInfo from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSignalInfo(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<byte> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedByteArray(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<int> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedInt32Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<long> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedInt64Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<float> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedFloat32Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<double> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedFloat64Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<string> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedStringArray(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<Vector2> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedVector2Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<Vector3> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedVector3Array(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Span<Color> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromPackedColorArray(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Godot.Object[]? from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemArrayOfGodotObject(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<TKey, TValue>(Collections.Dictionary<TKey, TValue> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromDictionary(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<T>(Collections.Array<T> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromArray(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<TKey, TValue>(System.Collections.Generic.Dictionary<TKey, TValue> from)
        where TKey : notnull => CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemDictionary(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<T>(System.Collections.Generic.List<T> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemICollection(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<TKey, TValue>(System.Collections.Generic.IDictionary<TKey, TValue> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemGenericIDictionary(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<T>(System.Collections.Generic.ICollection<T> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemGenericICollection(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From<T>(System.Collections.Generic.IEnumerable<T> from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemGenericIEnumerable(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Godot.Object from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromGodotObject(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(StringName from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromStringName(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(NodePath from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromNodePath(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(RID from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromRID(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Collections.Dictionary from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromDictionary(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static implicit operator Variant(Collections.Array from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromArray(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From(System.Collections.IDictionary from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemIDictionary(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From(System.Collections.ICollection from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemICollection(from));

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static Variant From(System.Collections.IEnumerable from) =>
        CreateTakingOwnershipOfDisposableValue(VariantUtils.CreateFromSystemIEnumerable(from));
}
