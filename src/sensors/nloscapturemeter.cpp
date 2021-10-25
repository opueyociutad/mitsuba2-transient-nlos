#include <mitsuba/core/fwd.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/rfilter.h>
#include <mitsuba/core/transform.h>
#include <mitsuba/core/warp.h>
#include <mitsuba/render/emitter.h>
#include <mitsuba/render/fwd.h>
#include <mitsuba/render/ior.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/sensor.h>

NAMESPACE_BEGIN(mitsuba)

/**!

.. _sensor-nloscapturemeter:

NLOS capture meter (:monosp:`nloscapturemeter`)
--------------------------------------------

.. pluginparameters::

 * - none

This class implements an interface to make radiance measurements in
specific points of the scene geometry's attached `shape` using
a typical laser-sensor setup.

For example, NLOS capture systems measure multiple evenly-spaced points on a
planar wall by aiming a capture sensor to a specific point during multiple
experiments, which produces a time-resolved image.

To use this plugin, the sensor must be defined as a child object to a shape
in a scene (e.g. square relay wall). The sensor should have the following
children:

 * A `film`, which will decide the distribution of the sensor measurements.
   Points on the <tt>[0,1]^2</tt> domain are divided into `width * height`
   (think of it as the pixels of a 2D image) which will be sampled as positions
   in the replay `shape` (the `rectangle` shape has good behaviour for planar
   walls). The sensor will sample the center of each of these pixels. A `box`
   reconstruction filter should likely be used, with the `high_quality_edges`
   option turned off.
 * A `emitter` (likely a projector with small field of view, to simulate a
   laser) which will be positioned and pointed as specified by `laser_origin`
   and `laser_lookat_pixel`
 * `sensor_origin`: Generated rays from the camera/sensor will start at this
   point and be directed at different points in the relay `shape`.
 * `laser_origin`: Position of the light
 * `laser_lookat_pixel`: Position of the relay `shape` where the laser will be
   pointed, specified in "pixels" as seen by the `film`. Example: for a 16x32
   film in a plane, a laser pointed to (7.5, 15.5) will point to the middle of
   the plane. Note that the X, Y coordinates are 0-indexed and the Z coordinate
   should be 0.
 * `account_first_and_last_bounces`: If false, the time it takes for light
   to travel (laser --> relay wall + relay_wall --> sensor) is substracted.

.. code-block:: xml
    :name: nlos-meter-example

    <shape type="plane">
        <sensor type="nloscapturemeter">
            <!-- sampler -->
            <boolean name="account_first_and_last_bounces" value="false"/>
            <point name="sensor_origin" x="1" y="0" z="0" />
            <point name="laser_origin" x="1" y="0" z="0" />
            <point name="laser_lookat_pixel" x="9" y="15" z="0" />
            <!-- film (recommended: streakhdrfilm) -->
            <!-- emitter (recommended: projector) -->
        </sensor>
    </shape>
*/

template <typename Float, typename Spectrum>
class NLOSCaptureMeter final : public Sensor<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(Sensor, m_film, m_world_transform, m_shape)
    MTS_IMPORT_TYPES(Scene, Shape, Emitter, ReconstructionFilter)

    NLOSCaptureMeter(const Properties &props) : Base(props) {
        if (m_film->reconstruction_filter()->radius() >
            0.5f + math::RayEpsilon<Float>)
            Log(Warn,
                "This sensor should only be used with a reconstruction filter"
                "of radius 0.5 or lower(e.g. default box)");

        for (auto &[name, obj] : props.objects(false)) {
            auto emitter = dynamic_cast<Emitter *>(obj.get());

            if (emitter) {
                if (m_emitter)
                    Throw("Only one emitter can be specified per NLOS capture "
                          "meter.");
                m_emitter = emitter;

                props.mark_queried(name);
            }
        }

        m_account_first_and_last_bounces =
            props.bool_("account_first_and_last_bounces", true);

        // Update the to_world transform if origin is also provided
        // NOTE(diego): const_cast bad, don't care
        const_cast<AnimatedTransform *>(m_world_transform.get())
            ->append(0.f, Transform4f::translate(
                              props.point3f("sensor_origin", 0.f)));

        m_laser_origin        = props.point3f("laser_origin", 0.f);
        Point3f laser_lookat3 = props.point3f("laser_lookat_pixel", 0.f);
        auto [film_width, film_height] = film_size();
        if (laser_lookat3.x() < 0 || film_width < laser_lookat3.x())
            Log(Warn, "Laser lookat pixel (X position) is out of bounds");
        if (laser_lookat3.y() < 0 || film_height < laser_lookat3.y())
            Log(Warn, "Laser lookat pixel (Y position) is out of bounds");
        if (abs(laser_lookat3.z()) > math::Epsilon<float>)
            Log(Warn, "Laser lookat pixel (Z position) should be 0");
        Point2f laser_lookat_pixel(laser_lookat3.x(), laser_lookat3.y());
        m_target_lookat_sample = pixel_to_sample(laser_lookat_pixel);

        auto pmgr = PluginManager::instance();
        if (!m_emitter) {
            Log(Warn, "This sensor should include a (projector) emitter. "
                      "Adding a default one, but this is probably not what "
                      "you want.");
            Properties props_film("projector");
            m_emitter = static_cast<Emitter *>(
                pmgr->create_object<Emitter>(props_film));
        }
    }

private:
    void set_scene(const Scene *scene) override {
        // NOTE(diego): const_cast bad, don't care
        auto &scene_emitters =
            const_cast<std::vector<ref<Emitter>> &>(scene->emitters());
        if (scene_emitters.size() != 0)
            Log(Warn, "This sensor only supports exactly one emitter object, "
                      "which should be placed inside the sensor. Please remove "
                      "other emitters.");
        scene_emitters.push_back(m_emitter);
    }

    void set_shape(Shape *shape) override {
        Base::set_shape(shape);

        Point3f laser_target =
            m_shape->sample_position(0.f, m_target_lookat_sample).p;
        // NOTE(diego): const_cast bad, don't care
        const_cast<AnimatedTransform *>(m_emitter->world_transform())
            ->append(0.f, Transform4f::look_at(m_laser_origin, laser_target,
                                               Vector3f(0.f, 1.f, 0.f)));

        m_laser_bounce_opl =
            norm(laser_target - m_laser_origin) * lookup_ior("air");
    }

    // TODO(diego): traverse function?

private:
    std::pair<uint, uint> film_size() const {
        ScalarPoint2i size = m_film->size();
        return std::make_pair(size.x(), size.y());
    }

    Point3f sensor_origin(Float time, Mask active) const {
        return m_world_transform->eval(time, active).translation();
    }

    Point2f pixel_to_sample(const Point2f &pixel) const {
        auto [film_width, film_height] = film_size();
        return Point2f{ (pixel.x() + 0.5f) / film_width,
                        (pixel.y() + 0.5f) / film_height };
    }

    std::pair<Float, Vector3f>
    sample_direction(Float time, const Point2f &sample, Mask active) const {
        Point3f origin = sensor_origin(time, active);

        auto [film_width, film_height] = film_size();
        // instead of continuous samples over the whole shape,
        // discretize samples so they only land on the center of the film's
        // "pixels"
        Point2f grid_sample = pixel_to_sample(Point2f{
            floor(sample.x() * film_width), floor(sample.y() * film_height) });
        Point3f target = m_shape->sample_position(time, grid_sample, active).p;

        Vector3f direction = target - origin;
        Float distance     = norm(direction);
        direction /= distance;
        return std::make_pair(distance, direction);
    }

public:
    std::pair<RayDifferential3f, Spectrum> sample_ray_differential(
        Float time, Float wavelength_sample, const Point2f &sample2,
        const Point2f & /* sample3 */, Mask active) const override {

        MTS_MASKED_FUNCTION(ProfilerPhase::EndpointSampleRay, active);

        Point3f origin = sensor_origin(time, active);
        auto [sensor_distance, direction] =
            sample_direction(time, sample2, active);

        auto [wavelengths, wav_weight] =
            sample_wavelength<Float, Spectrum>(wavelength_sample);

        if (!m_account_first_and_last_bounces)
            time -= m_laser_bounce_opl + sensor_distance * lookup_ior("air");

        return std::make_pair(
            RayDifferential3f(origin, direction, time, wavelengths),
            unpolarized<Spectrum>(wav_weight) * math::Pi<ScalarFloat>);
    }

    std::pair<DirectionSample3f, Spectrum>
    sample_direction(const Interaction3f &it, const Point2f &sample,
                     Mask active) const override {
        return std::make_pair(m_shape->sample_direction(it, sample, active),
                              math::Pi<ScalarFloat>);
    }

    Float pdf_direction(const Interaction3f & /*it*/,
                        const DirectionSample3f & /*ds*/,
                        Mask active) const override {
        // return m_shape->pdf_direction(it, ds, active);
        MTS_MASK_ARGUMENT(active);
        return Float(1.f);
    }

    Spectrum eval(const SurfaceInteraction3f & /*si*/,
                  Mask /*active*/) const override {
        return math::Pi<ScalarFloat> / m_shape->surface_area();
    }

    // TODO(diego): check if I should change bbox
    ScalarBoundingBox3f bbox() const override { return m_shape->bbox(); }

    std::string to_string() const override {
        std::ostringstream oss;
        oss << "NLOSCaptureMeter[" << std::endl
            << "  shape = " << m_shape << "," << std::endl
            << "  emitter = " << m_emitter << "," << std::endl
            << "  film = " << m_film << "," << std::endl
            << "]";
        return oss.str();
    }

    MTS_DECLARE_CLASS()

private:
    ref<Emitter> m_emitter;
    Point3f m_laser_origin;
    Point2f m_target_lookat_sample;
    bool m_account_first_and_last_bounces;
    Float m_laser_bounce_opl;
};

MTS_IMPLEMENT_CLASS_VARIANT(NLOSCaptureMeter, Sensor)
MTS_EXPORT_PLUGIN(NLOSCaptureMeter, "NLOSCaptureMeter")
NAMESPACE_END(mitsuba)