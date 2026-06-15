import enum
import PySide6.QtCore
import PySide6.QtGui
from shiboken6 import Shiboken
from typing import ClassVar

class GlassesType(enum.Enum):
    Unknown = ...
    Ella = ...
    Air = ...
    Flora = ...
    Gf = ...
    Gina = ...
    Hylla = ...

class SensorType(enum.IntFlag):
    Unknown = ...
    Basler = ...
    Imu = ...
    Slam = ...
    Rgb = ...
    Display = ...
    Proximity = ...
    AmbientLight = ...

class Glasses(PySide6.QtCore.QObject):
    imuUpdated: ClassVar[
        PySide6.QtCore.Signal
    ] = ...  # imuUpdated(const QSharedPointer<ImuData> imu_data)
    camUpdated: ClassVar[
        PySide6.QtCore.Signal
    ] = ...  # camUpdated(SensorType type, const ImagePair img_pair)
    proximityUpdated: ClassVar[
        PySide6.QtCore.Signal
    ] = ...  # proximityUpdated(const QSharedPointer<ProximityData> proximity_data)
    ambientLightUpdated: ClassVar[
        PySide6.QtCore.Signal
    ] = ...  # ambientLightUpdated(const QSharedPointer<AmbientLightData> al_data)

    def open(self) -> bool:
        """
        连接眼镜。在连接成功后方可调用其他接口。

        连接成功后，各传感器默认不会被启动。可调用`startSensors`按需启动传感器。

        Returns:
            bool: 连接是否成功
        """
        ...

    def close(self) -> bool:
        """
        断开眼镜连接

        Returns:
            bool: 断开是否成功
        """
        ...

    def isOpened(self) -> bool:
        """
        判断眼镜是否已连接

        Returns:
            bool: 是否已连接
        """
        ...

    def sensorNum(self, sensor_type: SensorType, /) -> int:
        """
        获取指定类型传感器的数量

        Args:
            sensor_type (SensorType): 传感器类型。见`SensorType`

        Returns:
            int: 传感器数量
        """
        ...

    def hasSensor(self, sensor_type: SensorType, /) -> bool:
        """
        判断眼镜是否有指定类型的传感器

        Args:
            sensor_type (SensorType): 传感器类型。见`SensorType`

        Returns:
            bool: 是否有指定类型的传感器
        """
        ...

    def startSensors(self, sensors: set[SensorType], /) -> bool:
        """
        启动指定类型传感器的服务。所有控制传感器的接口均需在对应服务启动后方可调用

        Args:
            sensors (SensorType): 需启动服务的传感器类型

        Returns:
            bool: 启动是否成功
        """
        ...

    def stopSensors(self, sensors: set[SensorType], /) -> bool:
        """
        停止指定类型传感器的服务

        Args:
            sensors (SensorType): 需停止服务的传感器类型

        Returns:
            bool: 停止是否成功
        """
        ...

    def activeSensors(self) -> set[SensorType]:
        """
        获取当前已启动服务的传感器类型

        Returns:
            SensorType: 已启动服务的传感器类型
        """
        ...

    def type(self) -> GlassesType:
        """
        获取眼镜类型。出于简化的目的：

        - Air/P55E/P55F均视为Air
        - GinaA/GinaB均视为Gina
        - HyllaA/HyllaB均视为Hylla

        Returns:
            GlassesType: 眼镜类型。见`GlassesType`
        """
        ...

    def fsn(self) -> str:
        """
        获取眼镜的FSN

        Returns:
            str: 眼镜的FSN，无连接时返回`UnknownFSN`, 眼镜无FSN时返回`_XXX_NO_FSN_`（XXX为眼镜类型）
        """
        ...

    def mcuFirmwareVersion(self) -> str:
        """
        获取眼镜MCU固件版本

        Returns:
            str: 眼镜MCU固件版本
        """
        ...

    def display(self) -> GlassesDisplay:
        """
        获取控制眼镜光机的对象。需通过`startSensors`启动Display服务后方可调用此接口，否则返回空指针。

        Returns:
            GlassesDisplay: 控制眼镜光机的对象
        """
        ...

    def enableProximitySensor(self, enabled: bool, /) -> bool:
        """
        启用/禁用距离传感器。需通过`startSensors`启动Proximity服务后方可调用此接口。**此接口目前仅在Flora眼镜上有效**

        Args:
            enabled (bool): 启用/禁用距离传感器

        Returns:
            bool: 操作是否成功
        """
        ...

    def extraCpuLoad(self) -> int: ...
    def setExtraCpuLoad(self, load_percent: int, /) -> bool:
        """
        对眼镜施加额外的CPU负载。**此接口仅在X1芯片眼镜（Gf/Gina/Hylla）上有效**

        注意，CPU负载占比是通过占用的CPU周期近似的，非严格控制。比如，施加70%的负载时，在眼镜内通过
        top观察到的CPU占用可能在50-70%不等

        Args:
            load_percent (int): 负载百分比，范围[0, 100]

        Returns:
            bool: 操作是否成功
        """
        ...

    def electrochromicLevel(self) -> int: ...
    def setElectrochromicLevel(self, level: int, /) -> bool:
        """
        设置电致变色档位

        Args:
            level (int): 范围[0, 3]，0透明，3全黑

        Returns:
            bool: 设置是否成功
        """
        ...

    def setAutoExposure(self, cam_type: SensorType, auto_exposure: bool, /) -> bool:
        """
        设置相机自动曝光。由于固件的支持问题，自动曝光/曝光/增益等值均只支持设置，不支持查询，且相关设置接口暂不支持Hylla眼镜

        Args:
            cam_type (SensorType): 相机类型，需为Slam/Rgb
            auto_exposure (bool): 是否自动曝光

        Returns:
            bool: 设置是否成功
        """
        ...

    def setExposure(self, cam_type: SensorType, exposure_us: int, /) -> bool:
        """
        设置相机曝光值。此函数的限制同setAutoExposure`

        Args:
            cam_type (SensorType): 相机类型，需为Slam/Rgb
            exposure_us (int): 曝光值，单位为微秒

        Returns:
            bool: 设置是否成功
        """
        ...

    def setGain(self, cam_type: SensorType, gain: float, /) -> bool:
        """
        设置相机增益值

        Args:
            cam_type (SensorType): 相机类型，需为Slam/Rgb
            gain (float): 增益值。由于眼镜型号、相机类型的差异，有效值的范围会有差异：
                - 对于Flora的Slam相机，范围是[1, 7]
                - 对于G系列的Gem（RGB）相机，范围是[1, 16]

        Returns:
            bool: 设置是否成功
        """
        ...

    def setFrameRate(self, cam_type: SensorType, frame_rate: float, /) -> bool:
        """
        设置相机帧率。此接口仅在X1芯片眼镜（Gf/Gina/Hylla）上有效。

        此接口实际上是控制底层传输相机图片的帧率，从而降低数据传输占用的带宽。设计此接口的原因是，
        G系列的USB接口在传输数据时仅有USB 2.0的速度，如果按实际拍摄的帧率传输，则将出现很大的延迟
        和内存占用。

        Args:
            cam_type (SensorType): 相机类型，需为Slam/Rgb
            frame_rate (float): 帧率。最大值视相机类型而定，通常不高于30Hz。但实际应用中，如果要达到较低的延迟，则通常不高于15Hz

        Returns:
            bool: 设置是否成功
        """
        ...

    def rgbCamSn(self) -> str:
        """
        获取RGB（Gem）相机SN。仅对Gem相机有效

        Returns:
            str: Gem相机SN
        """
        ...

    def rgbCamConfig(self) -> str:
        """
        获取RGB（Gem）相机标定结果JSON。仅对Gem相机有效

        Returns:
            str: Gem相机标定结果JSON
        """
        ...

    def setRgbCamConfig(self, string: str, /) -> bool:
        """
        写入RGB（Gem）相机标定结果JSON。仅对Gem相机有效

        Args:
            string (str): 需写入的JSON字符串

        Returns:
            bool: 写入是否成功
        """
        ...

    def glassesConfig(self) -> str:
        """
        获取眼镜标定结果JSON

        Returns:
            str: 眼镜标定结果JSON
        """
        ...

    def setGlassesConfig(self, config_json: str, /) -> bool:
        """
        写入眼镜标定结果JSON

        Args:
            config_json (str): 需写入的JSON字符串

        Returns:
            bool: 写入是否成功
        """
        ...

class GlassesDisplay(PySide6.QtCore.QObject):
    def open(self, open_display: bool, /) -> bool:
        """
        打开/关闭光机

        Args:
            open_display (bool): 打开/关闭光机

        Returns:
            bool: 操作是否成功
        """
        ...

    def resolution(self) -> PySide6.QtCore.QSize:
        """
        获取光机分辨率

        Returns:
            PySide6.QtCore.QSize: 光机分辨率
        """
        ...

    def lightness(self) -> int: ...
    def setLightness(self, lightness: int, /) -> bool:
        """
        设置光机亮度档位

        Args:
            lightness (int): 亮度档位，范围[0, 7]。对应的尼特值与眼镜类型有关

        Returns:
            bool: 设置是否成功
        """
        ...

    def screenNitRange(self) -> tuple[int, int]:
        """
        获取光机尼特值范围
        """
        ...
    def screenNit(self) -> int:
        """
        获取光机当前尼特值
        """
        ...
    def setScreenNit(self, nit: int, /) -> bool:
        """
        设置光机尼特值

        Args:
            nit (int): 尼特值, 有效范围视眼镜类型和固件有所区分，具体可使用`screenNitRange`接口查询

        Returns:
            bool: 设置是否成功
        """
        ...

    def dutyRatio(self) -> int: ...
    def setDutyRatio(self, duty_ratio: int, /) -> bool:
        """
        设置光机占空比

        Args:
            duty_ratio (int): 占空比，范围[0, 100]

        Returns:
            bool: 设置是否成功
        """
        ...

    def screenShifts(self) -> ScreenShifts: ...
    def setScreenShifts(self, shifts: ScreenShifts, /) -> bool:
        """
        设置屏幕调像素。此接口目前仅对Air/Flora眼镜有效（P55系列眼镜XrGlasses库视同Air）

        Args:
            shifts (ScreenShifts): 调像素值

        Returns:
            bool: 设置是否成功
        """
        ...

    def colorTemperature(self) -> list[int]:
        """
        获取光机色温值

        Returns:
            list[int]: 色温值，依次为左屏X轴、左屏Y轴、右屏X轴、右屏Y轴的值

        """
        ...

    def setColorTemperature(
        self, left_h: int, left_v: int, right_h: int, right_v: int, /
    ) -> bool:
        """
        设置光机色温值

        Args:
            left_h (int): 左屏X轴色温值
            left_v (int): 左屏Y轴色温值
            right_h (int): 右屏X轴色温值
            right_v (int): 右屏Y轴色温值

        Returns:
            bool: 设置是否成功
        """
        ...

    def showImage(self, image: PySide6.QtGui.QImage, /) -> bool:
        """
        将图像全屏显示在光机上

        Args:
            image (PySide6.QtGui.QImage): 需显示的图像

        Returns:
            bool: 投屏是否成功
        """
        ...

    def showImageByGray(self, gray: int, /) -> bool:
        """
        生成一张对应光机分辨率的图像，以gray指定的灰度填充全图，最后将图像全屏显示在光机上

        Args:
            gray (int): 生成图像需填充的灰度值，范围[0, 255]

        Returns:
            bool: 投屏是否成功
        """
        ...

    def closeImage(self) -> bool:
        """
        关闭正显示在眼镜光机上的图像窗口

        Returns:
            bool: 操作是否成功
        """
        ...

    def isShowingImage(self) -> bool:
        """
        判断当前是否有投图显示

        Returns:
            bool: 是否正在显示图像
        """
        ...

class GlassesFactory(PySide6.QtCore.QObject):
    @staticmethod
    def instance() -> GlassesFactory: ...
    def createGlasses(self, product_id: int, /) -> Glasses: ...
    def enumerateDevices(self) -> list[int]: ...

class BspImage(Shiboken.Object):
    image: PySide6.QtGui.QImage
    exposure_start_time_device: int
    exposure_start_time_system: int
    exposure_duration: int
    rolling_shutter_time: int
    stride: int
    gain: int
    def __init__(self) -> None: ...
    def __bool__(self) -> bool: ...
    def __copy__(self) -> BspImage: ...
    def isNull(self) -> bool: ...

class ImagePair(Shiboken.Object):
    def __init__(self) -> None: ...
    def __bool__(self) -> bool: ...
    def __copy__(self) -> ImagePair: ...
    def addImage(
        self, cam_index: int, timestamp: int, bsp_image: BspImage, /
    ) -> None: ...
    def bspImageAt(self, index: int, /) -> BspImage: ...
    def imageAt(self, index: int, /) -> PySide6.QtGui.QImage: ...
    def isNull(self) -> bool: ...
    def left(self) -> PySide6.QtGui.QImage: ...
    def right(self) -> PySide6.QtGui.QImage: ...
    def timestamp(self) -> int: ...
    def timeDiffTolerance(self) -> int: ...
    def setTimeDiffTolerance(self, tolerance: int, /) -> None: ...

class ImuData(Shiboken.Object):
    class Mask(enum.IntEnum):
        Null = ...
        Gyro = ...
        Acc = ...
        Mag = ...

    imu_idx: int
    hmd_time_ns: int
    hmd_hw_time_ns: int
    hmd_sensor_time_ns: int
    gyro: list[float]
    acc: list[float]
    mag: list[float]
    temperature: float
    mask: Mask
    def __init__(self) -> None: ...
    def __copy__(self) -> ImuData: ...
    def hasAcc(self) -> bool: ...
    def hasGyro(self) -> bool: ...
    def hasMag(self) -> bool: ...

class ProximityData(Shiboken.Object):
    class ChannelData(Shiboken.Object):
        ph: int
        diff: int
        useful: int
        ph4_use: int
        ph5_use: int
        ph6_use: int
        offset: int
        average: int
        def __init__(self) -> None: ...
        def __copy__(self) -> ProximityData.ChannelData: ...

    class State(enum.IntEnum):
        Unknown = ...
        Wearing = ...
        NotWearing = ...

    timestamp: int
    wearing_state: ProximityData.State
    channel_data: list[ProximityData.ChannelData]
    def __init__(self) -> None: ...
    def __copy__(self) -> ProximityData: ...

class AmbientLightData(Shiboken.Object):
    hmd_time_nanos_system: int
    hmd_time_nanos_device: int
    frame_id: int
    lux: int
    f_value: int
    r_value: int
    b_value: int
    g_value: int
    c_value: int
    def __init__(self) -> None: ...
    def __copy__(self) -> AmbientLightData: ...

class QIntList:
    __opaque_container__: ClassVar[bool] = ...
    def __init__(self, *args, **kwargs) -> None:
        """Initialize self.  See help(type(self)) for accurate signature."""

    def append(self, *args, **kwargs):
        """append"""

    def capacity(self, *args, **kwargs):
        """capacity"""

    def clear(self, *args, **kwargs):
        """clear"""

    def constData(self, *args, **kwargs):
        """constData"""

    def data(self, *args, **kwargs):
        """data"""

    def pop_back(self, *args, **kwargs):
        """pop_back"""

    def pop_front(self, *args, **kwargs):
        """pop_front"""

    def prepend(self, *args, **kwargs):
        """prepend"""

    def push_back(self, *args, **kwargs):
        """push_back"""

    def push_front(self, *args, **kwargs):
        """push_front"""

    def removeFirst(self, *args, **kwargs):
        """removeFirst"""

    def removeLast(self, *args, **kwargs):
        """removeLast"""

    def reserve(self, *args, **kwargs):
        """reserve"""

    def __delitem__(self, other) -> None:
        """Delete self[key]."""

    def __getitem__(self, index):
        """Return self[key]."""

    def __len__(self) -> int:
        """Return len(self)."""

    def __setitem__(self, index, object) -> None:
        """Set self[key] to value."""

class QSharedPointer_ImuData(Shiboken.Object):
    def __init__(self) -> None: ...
    def data(self) -> ImuData: ...
    def reset(self) -> None: ...
    def __bool__(self) -> bool:
        """True if self else False"""

    def __copy__(self):
        """__copy__(self, /) -> Self"""

    def __delattr__(self, name):
        """Implement delattr(self, name)."""

    def __dir__(self) -> None:
        """__dir__() -> None"""

    def __eq__(self, other: object) -> bool:
        """Return self==value."""

    def __ge__(self, other: object) -> bool:
        """Return self>=value."""

    def __gt__(self, other: object) -> bool:
        """Return self>value."""

    def __le__(self, other: object) -> bool:
        """Return self<=value."""

    def __lt__(self, other: object) -> bool:
        """Return self<value."""

    def __ne__(self, other: object) -> bool:
        """Return self!=value."""

    def __setattr__(self, name, value):
        """Implement setattr(self, name, value)."""

class QSharedPointer_ProximityData(Shiboken.Object):
    def __init__(self) -> None: ...
    def data(self) -> ProximityData: ...
    def reset(self) -> None: ...
    def __bool__(self) -> bool:
        """True if self else False"""

    def __copy__(self):
        """__copy__(self, /) -> Self"""

    def __delattr__(self, name):
        """Implement delattr(self, name)."""

    def __dir__(self) -> None:
        """__dir__() -> None"""

    def __eq__(self, other: object) -> bool:
        """Return self==value."""

    def __ge__(self, other: object) -> bool:
        """Return self>=value."""

    def __gt__(self, other: object) -> bool:
        """Return self>value."""

    def __le__(self, other: object) -> bool:
        """Return self<=value."""

    def __lt__(self, other: object) -> bool:
        """Return self<value."""

    def __ne__(self, other: object) -> bool:
        """Return self!=value."""

    def __setattr__(self, name, value):
        """Implement setattr(self, name, value)."""

class QSharedPointer_AmbientLightData(Shiboken.Object):
    def __init__(self) -> None: ...
    def data(self) -> AmbientLightData: ...
    def reset(self) -> None: ...
    def __bool__(self) -> bool:
        """True if self else False"""

    def __copy__(self):
        """__copy__(self, /) -> Self"""

    def __delattr__(self, name):
        """Implement delattr(self, name)."""

    def __dir__(self) -> None:
        """__dir__() -> None"""

    def __eq__(self, other: object) -> bool:
        """Return self==value."""

    def __ge__(self, other: object) -> bool:
        """Return self>=value."""

    def __gt__(self, other: object) -> bool:
        """Return self>value."""

    def __le__(self, other: object) -> bool:
        """Return self<=value."""

    def __lt__(self, other: object) -> bool:
        """Return self<value."""

    def __ne__(self, other: object) -> bool:
        """Return self!=value."""

    def __setattr__(self, name, value):
        """Implement setattr(self, name, value)."""

class ScreenShift(Shiboken.Object):
    horizontal: float
    vertical: float
    def __init__(self) -> None: ...
    def isNull(self) -> bool: ...
    def __bool__(self) -> bool: ...
    def __copy__(self) -> ScreenShift: ...
    def __eq__(self, other: ScreenShift, /) -> bool: ...
    def __ge__(self, other: ScreenShift, /) -> bool: ...
    def __gt__(self, other: ScreenShift, /) -> bool: ...
    def __le__(self, other: ScreenShift, /) -> bool: ...
    def __lt__(self, other: ScreenShift, /) -> bool: ...
    def __ne__(self, other: ScreenShift, /) -> bool: ...

class ScreenShifts(Shiboken.Object):
    left: ScreenShift
    right: ScreenShift
    def __init__(self) -> None: ...
    def isNull(self) -> bool: ...
    def roundedValues(self) -> list[int]: ...
    def values(self) -> list[float]: ...
    def __bool__(self) -> bool: ...
    def __copy__(self) -> ScreenShifts: ...
    def __eq__(self, other: ScreenShifts, /) -> bool: ...
    def __ge__(self, other: ScreenShifts, /) -> bool: ...
    def __gt__(self, other: ScreenShifts, /) -> bool: ...
    def __le__(self, other: ScreenShifts, /) -> bool: ...
    def __lt__(self, other: ScreenShifts, /) -> bool: ...
    def __ne__(self, other: ScreenShifts, /) -> bool: ...

def initLogging(name: str, /) -> None: ...
def isCamera(sensor_type: SensorType, /) -> bool: ...
def minimumSensorNum(glasses_type: GlassesType, sensor_type: SensorType, /) -> int: ...
