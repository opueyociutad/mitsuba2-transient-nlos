#pragma once

#include <mitsuba/core/fwd.h>
#include <mitsuba/core/object.h>
#include <mitsuba/core/properties.h>
#include <mitsuba/core/spectrum.h>
#include <mitsuba/core/timer.h>
#include <mitsuba/core/tls.h>
#include <mitsuba/core/vector.h>
#include <mitsuba/render/fwd.h>
#include <mitsuba/render/streakimageblock.h>
#include <mitsuba/render/integrator.h>
#include <mitsuba/render/interaction.h>
#include <mitsuba/render/medium.h>
#include <mitsuba/render/radiancesample.h>
#include <mitsuba/render/records.h>
#include <mitsuba/render/scene.h>
#include <mitsuba/render/shape.h>

NAMESPACE_BEGIN(mitsuba)

template <typename Float, typename Spectrum>
class MTS_EXPORT_RENDER TransientIntegrator
    : public Integrator<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(Integrator)
    MTS_IMPORT_TYPES(Scene, Sensor)

    /// Perform the main rendering job. Returns \c true upon success
    bool render(Scene *scene, Sensor *sensor) override = 0;

    /**
     * \brief Cancel a running render job
     *
     * This function can be called asynchronously to cancel a running render
     * job. In this case, \ref render() will quit with a return value of \c
     * false.
     */
    void cancel() override = 0;

    MTS_DECLARE_CLASS()
protected:
    /// Create an integrator
    TransientIntegrator(const Properties & /*props*/);

    /// Virtual destructor
    virtual ~TransientIntegrator();
};

template <typename Float, typename Spectrum>
class MTS_EXPORT_RENDER TransientSamplingIntegrator
    : public TransientIntegrator<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(TransientIntegrator)
    MTS_IMPORT_TYPES(Scene, Sensor, StreakFilm, StreakImageBlock, Medium, Sampler)

    /**
     * \brief Sample the incident radiance along a ray.
     *
     * \param scene
     *    The underlying scene in which the radiance function should be sampled
     *
     * \param sampler
     *    A source of (pseudo-/quasi-) random numbers
     *
     * \param ray
     *    A ray, optionally with differentials
     *
     * \param medium
     *    If the ray is inside a medium, this parameter holds a pointer to that
     *    medium
     *
     * \param active
     *    A mask that indicates which SIMD lanes are active
     *
     * \param aov
     *    Integrators may return one or more arbitrary output variables (AOVs)
     *    via this parameter. If \c nullptr is provided to this argument, no
     *    AOVs should be returned. Otherwise, the caller guarantees that space
     *    for at least <tt>aov_names().size()</tt> entries has been allocated.
     *
     * \param radiance
     *    Integrator must return a pair containing a spectrum and a mask
     *    specifying whether a surface or medium interaction was sampled.
     *    False mask entries indicate that the ray "escaped" the scene, in
     *    which case the the returned spectrum contains the contribution of
     *    environment maps, if present. The mask can be used to estimate a
     *    suitable alpha channel of a rendered image.
     *
     * \remark
     *    In the Python bindings, this function returns the \c aov output
     *    argument as an additional return value. In other words:
     *    <tt>
     *        (spec, mask, aov) = integrator.sample(scene, sampler, ray, medium,
     * active)
     *    </tt>
     */
    virtual void sample(const Scene *scene,
                        Sampler *sampler,
                        const RayDifferential3f &ray,
                        const Medium *medium = nullptr,
                        Float *aovs = nullptr,
                        Mask active = true,
                        std::vector<RadianceSample<Float, Spectrum, Mask>> &radianceSamplesRecordVector = {}) const;

    /**
     * For integrators that return one or more arbitrary output variables
     * (AOVs), this function specifies a list of associated channel names. The
     * default implementation simply returns an empty vector.
     */
    virtual std::vector<std::string> aov_names() const;

    // =========================================================================
    //! @{ \name Integrator interface implementation
    // =========================================================================

    bool render(Scene *scene, Sensor *sensor) override;
    void cancel() override;

    /**
     * Indicates whether \ref cancel() or a timeout have occured. Should be
     * checked regularly in the integrator's main loop so that timeouts are
     * enforced accurately.
     *
     * Note that accurate timeouts rely on \ref m_render_timer, which needs
     * to be reset at the beginning of the rendering phase.
     */
    bool should_stop() const {
        return m_stop ||
               (m_timeout > 0.f && m_render_timer.value() > 1000.f * m_timeout);
    }

    //! @}
    // =========================================================================

    MTS_DECLARE_CLASS()
protected:
    TransientSamplingIntegrator(const Properties &props);
    virtual ~TransientSamplingIntegrator();

    virtual void render_block(const Scene *scene, const Sensor *sensor,
                              Sampler *sampler, StreakImageBlock *block, Float *aovs,
                              size_t sample_count, size_t block_id) const;

    void render_sample(const Scene *scene, const Sensor *sensor,
                       Sampler *sampler, StreakImageBlock *block, Float *aovs,
                       const Vector2f &pos, ScalarFloat diff_scale_factor,
                       Mask active = true) const;

protected:
    /// Integrators should stop all work when this flag is set to true.
    bool m_stop;

    /// Size of (square) image blocks to render per core.
    uint32_t m_block_size;

    /**
     * \brief Number of samples to compute for each pass over the image blocks.
     *
     * Must be a multiple of the total sample count per pixel.
     * If set to (size_t) -1, all the work is done in a single pass (default).
     */
    uint32_t m_samples_per_pass;

    /**
     * \brief Maximum amount of time to spend rendering (excluding scene
     * parsing).
     *
     * Specified in seconds. A negative values indicates no timeout.
     */
    float m_timeout;

    /// Timer used to enforce the timeout.
    Timer m_render_timer;

    /// Flag for disabling direct visibility of emitters
    bool m_hide_emitters;
};

/*
 * \brief Base class of all recursive Monte Carlo integrators, which compute
 * unbiased solutions to the rendering equation (and optionally the radiative
 * transfer equation).
 */
template <typename Float, typename Spectrum>
class MTS_EXPORT_RENDER TransientMonteCarloIntegrator
    : public TransientSamplingIntegrator<Float, Spectrum> {
public:
    MTS_IMPORT_BASE(TransientSamplingIntegrator)

protected:
    /// Create an integrator
    TransientMonteCarloIntegrator(const Properties &props);

    /// Virtual destructor
    virtual ~TransientMonteCarloIntegrator();

    MTS_DECLARE_CLASS()
protected:
    int m_max_depth;
    int m_rr_depth;
};

MTS_EXTERN_CLASS_RENDER(TransientIntegrator)
MTS_EXTERN_CLASS_RENDER(TransientSamplingIntegrator)
MTS_EXTERN_CLASS_RENDER(TransientMonteCarloIntegrator)
NAMESPACE_END(mitsuba)