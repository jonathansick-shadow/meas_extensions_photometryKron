// -*- LSST-C++ -*-
#include <numeric>
#include <cmath>
#include <functional>
#include "boost/math/constants/constants.hpp"
#include "lsst/pex/exceptions.h"
#include "lsst/pex/logging/Trace.h"
#include "lsst/afw/geom/Point.h"
#include "lsst/afw/image.h"
#include "lsst/afw/math/Integrate.h"
#include "lsst/meas/algorithms/Measure.h"

#include "lsst/afw/detection/Psf.h"
#include "lsst/afw/detection/Photometry.h"
#include "lsst/meas/algorithms/detail/SdssShape.h"
#include "lsst/meas/algorithms/Photometry.h"

namespace pexExceptions = lsst::pex::exceptions;
namespace pexLogging = lsst::pex::logging;
namespace afwDetection = lsst::afw::detection;
namespace afwGeom = lsst::afw::geom;
namespace afwImage = lsst::afw::image;
namespace afwMath = lsst::afw::math;

double const PI = boost::math::constants::pi<double>();          // ~ 355/113.0
double const ROOT2 = boost::math::constants::root_two<double>(); // sqrt(2)

namespace lsst {
namespace meas {
namespace algorithms {

/**
 * @brief A class that knows how to calculate fluxes using the KRON photometry algorithm
 *
 * @ingroup meas/algorithms
 */
class KronPhotometry : public afwDetection::Photometry
{
    enum { RADIUS = Photometry::NVALUE,
           NVALUE = RADIUS + 1 };

public:
    typedef boost::shared_ptr<KronPhotometry> Ptr;
    typedef boost::shared_ptr<KronPhotometry const> ConstPtr;

    /// Ctor
    KronPhotometry(double radius, double flux, double fluxErr=std::numeric_limits<double>::quiet_NaN()) :
        afwDetection::Photometry() {
        init();                         // This allocates space for everything in the schema

        set<FLUX>(flux);
        set<FLUX_ERR>(fluxErr);
        set<RADIUS>(radius);
    }

    /// Add desired fields to the schema
    virtual void defineSchema(afwDetection::Schema::Ptr schema ///< our schema; == _mySchema
                     ) {
        Photometry::defineSchema(schema);
        schema->add(afwDetection::SchemaEntry("radius", RADIUS, afwDetection::Schema::DOUBLE, 1, "pixels"));
    }

    double getParameter(int) const {
        return get<RADIUS, double>();
    }

    static bool doConfigure(lsst::pex::policy::Policy const& policy)
    {
        if (policy.isDouble("nSigmaForRadius")) {
            _nSigmaForRadius = policy.getDouble("nSigmaForRadius");
        }
        if (policy.isDouble("nRadiusForFlux")) {
            _nRadiusForFlux = policy.getDouble("nRadiusForFlux");
        }
        if (policy.isDouble("background")) {
            _background = policy.getDouble("background");
        } 
        if (policy.isDouble("shiftmax")) {
            _shiftmax = policy.getDouble("shiftmax");
        } 
        
        return true;
    }
    template<typename ImageT>
    static Photometry::Ptr doMeasure(CONST_PTR(ImageT),
                                     CONST_PTR(afwDetection::Peak),
                                     CONST_PTR(afwDetection::Source)
                                    );

private:
    static double _nSigmaForRadius;
    static double _nRadiusForFlux;
    static double _background;
    static double _shiftmax;

    KronPhotometry(void) : afwDetection::Photometry() { }
    LSST_SERIALIZE_PARENT(afwDetection::Photometry)
};

LSST_REGISTER_SERIALIZER(KronPhotometry)

double KronPhotometry::_nSigmaForRadius = 6.0; // Size of aperture (in sigma) to estimate Kron radius
double KronPhotometry::_nRadiusForFlux = 2.0;  // number of R_Kron to measure flux in
double KronPhotometry::_background = 0.0;      // the frame's background level
double KronPhotometry::_shiftmax = 10;         // Max allowed centroid shift

/************************************************************************************************************/

namespace {
template <typename MaskedImageT, typename WeightImageT>
class FootprintFindMoment : public afwDetection::FootprintFunctor<MaskedImageT> {
public:
    FootprintFindMoment(MaskedImageT const& mimage, ///< The image the source lives in
                        double const xcen, double const ycen, // center of the object
                        double const ab,                      // axis ratio
                        double const theta                    // rotation of ellipse +ve from x axis
                       ) : afwDetection::FootprintFunctor<MaskedImageT>(mimage),
                           _xcen(xcen), _ycen(ycen),
                           _ab(ab),
                           _cosTheta(::cos(theta)),
                           _sinTheta(::sin(theta)),
                           _sum(0.0), _sumR(0.0), _sumRVar(0),
                           _imageX0(mimage.getX0()), _imageY0(mimage.getY0())
        {}
    
    /// @brief Reset everything for a new Footprint
    void reset() {}        
    void reset(afwDetection::Footprint const& foot) {
        _sum = _sumR = _sumRVar = 0.0;

        MaskedImageT const& mimage = this->getImage();
        afwGeom::Box2I const& bbox(foot.getBBox());
        int const x0 = bbox.getMinX(), y0 = bbox.getMinY(), x1 = bbox.getMaxX(), y1 = bbox.getMaxY();

        if (x0 < _imageX0 || y0 < _imageY0 ||
            x1 >= _imageX0 + mimage.getWidth() || y1 >= _imageY0 + mimage.getHeight()) {
            throw LSST_EXCEPT(lsst::pex::exceptions::OutOfRangeException,
                              (boost::format("Footprint %d,%d--%d,%d doesn't fit in image %d,%d--%d,%d")
                               % x0 % y0 % x1 % y1
                               % _imageX0 % _imageY0
                               % (_imageX0 + mimage.getWidth() - 1) % (_imageY0 + mimage.getHeight() - 1)
                              ).str());
        }
    }
    
    /// @brief method called for each pixel by apply()
    void operator()(typename MaskedImageT::xy_locator iloc, ///< locator pointing at the image pixel
                    int x,                                  ///< column-position of pixel
                    int y                                   ///< row-position of pixel
                   ) {
        typename MaskedImageT::Image::Pixel ival = iloc.image(0, 0);
        typename MaskedImageT::Variance::Pixel vval = iloc.variance(0, 0);

        double const dx = (x - _imageX0) - _xcen;
        double const dy = (y - _imageY0) - _ycen;
        double const du =  dx*_cosTheta + dy*_sinTheta;
        double const dv = -dx*_sinTheta + dy*_cosTheta;

        double r = ::hypot(du, dv*_ab); // ellipsoidal radius
#if 1
        if (dx*dx + dy*dy < 0.25) {     // within a pixel of the centre
            /*
             * We gain significant precision for flattened Gaussians by treating the central pixel specially
             *
             * If the object's centered in the pixel (and has constant surface brightness) we have <r> == eR;
             * if it's at the corner <r> = 2*eR; we interpolate between these exact results linearily in the
             * displacement.
             */
            
            double const eR = 0.38259771140356325; // <r> for a single square pixel, about the centre
            r = (eR/_ab)*(1 + ROOT2*::hypot(::fmod(du, 1), ::fmod(dv, 1)));
        }
#endif

        _sum += ival;
        _sumR += r*ival;
        _sumRVar += r*r*vval;
    }

    /// Return the Footprint's <r>
    double getIr() const { return _sumR/_sum; }

    /// Return the variance of the Footprint's <r>
    double getIrVar() const { return _sumRVar/_sum - getIr()*getIr(); }

private:
    double const _xcen;                 // center of object
    double const _ycen;                 // center of object
    double const _ab;                   // axis ratio
    double const _cosTheta, _sinTheta;  // {cos,sin}(angle from x-axis)
    double _sum;                        // sum of I
    double _sumR;                       // sum of R*I
    double _sumRVar;                    // sum of R*R*Var(I)
    int const _imageX0, _imageY0;       // origin of image we're measuring
};

}
/*
 * Do we have the ellipticalFootprint function?  In sufficiently recent versions of afw it's available
 * as a Footprint constructor;  when this is in a cut version this function should be deleted
 */
#define HAVE_ellipticalFootprint 1      // n.b. if == 0 you can clean up kronLib.i too

#if HAVE_ellipticalFootprint
/************************************************************************************************************/
/**
 * Create an elliptical Footprint.
 */
PTR(afwDetection::Footprint)
ellipticalFootprint(afwGeom::Point2I const& center, //!< The center of the circle
                    double a,                       //!< Major axis (pixels)
                    double b,                       //!< Minor axis (pixels)
                    double theta,                   //!< angle of major axis from x-axis; (radians)
                    afwGeom::Box2I const& region=afwGeom::Box2I() //!< Bounding box of MaskedImage footprint
                   )
{
    PTR(afwDetection::Footprint) foot(new afwDetection::Footprint);
    foot->setRegion(region);
    
    int const xc = center[0];           // x-centre
    int const yc = center[1];           // y-centre

    double const c = ::cos(theta);
    double const s = ::sin(theta);

    double const c0 = a*a*s*s + b*b*c*c;
    double const c1 = c*s*(a*a - b*b)/c0;
    double const c2 = a*b/c0;

    double const ymax = ::sqrt(c0) + 1; // max extent of ellipse in y-direction
    //
    // We go to quite a lot of annoying trouble to ensure that all pixels that are within or intercept
    // the ellipse are included in the Footprint
    //
    double x1, x2, y;
    for (int i = -ymax; i <= ymax; ++i) {
        double const dy = (i > 0) ? -0.5 : 0.5;
        y = i + dy;              // chord at top of pixel (above centre)
        if (c0 > y*y) {
            x1 = y*c1 - c2*std::sqrt(c0 - y*y);
            x2 = y*c1 + c2*std::sqrt(c0 - y*y);
        } else {
            x1 = x2 = y*c1;
        }

        y = i - dy;                     // chord at bottom of pixel (above centre)
        if (c0 > y*y) {
            double tmp = y*c1 - c2*std::sqrt(c0 - y*y);
            if (tmp < x1) {
                x1 = tmp;
            }

            tmp = y*c1 + c2*std::sqrt(c0 - y*y);
            if (tmp > x2) {
                x2 = tmp;
            }
        }

        foot->addSpan(yc + i, xc + x1 + 0.5, xc + x2 + 0.5);
    }

    return foot;
}
#endif

/************************************************************************************************************/
/**
 * Calculate the desired Kron radius and flux
 */
template<typename ExposureT>
afwDetection::Photometry::Ptr KronPhotometry::doMeasure(CONST_PTR(ExposureT) exposure,
                                                        CONST_PTR(afwDetection::Peak) peak,
                                                        CONST_PTR(afwDetection::Source) source
                                                       )
{
    double radius = std::numeric_limits<double>::quiet_NaN();
    double flux = std::numeric_limits<double>::quiet_NaN();
    double fluxErr = std::numeric_limits<double>::quiet_NaN();

    if (!peak) {
        return boost::make_shared<KronPhotometry>(radius, flux, fluxErr);
    }

    typedef typename ExposureT::MaskedImageT MaskedImageT;
    typedef typename MaskedImageT::Image Image;
    typedef typename Image::Pixel Pixel;
    typedef typename Image::Ptr ImagePtr;

    MaskedImageT const& mimage = exposure->getMaskedImage();
    
    double const xcen = peak->getFx() - mimage.getX0(); // object's column position in image pixel coords
    double const ycen = peak->getFy() - mimage.getY0();  // object's row position
    /*
     * Estimate the object's moments using the SDSS adaptive moments algorithm
     */
    double Ixx = std::numeric_limits<double>::quiet_NaN(); // <xx>
    double Ixy = std::numeric_limits<double>::quiet_NaN(); // <xy>
    double Iyy = std::numeric_limits<double>::quiet_NaN(); // <yy>
    try {
        if (source) {
            CONST_PTR(lsst::afw::detection::Measurement<lsst::afw::detection::Shape>)
                shape = source->getShape();
            
            Ixx = shape->find("SDSS")->getIxx();
            Ixy = shape->find("SDSS")->getIxy();
            Iyy = shape->find("SDSS")->getIyy();
        } else {
            throw LSST_EXCEPT(pexExceptions::NotFoundException, "Source is NULL");
        }
    } catch (pexExceptions::Exception& e) {
        detail::SdssShapeImpl shapeImpl;
        
        if (!detail::getAdaptiveMoments(mimage, _background, xcen, ycen, _shiftmax, &shapeImpl)) {
            std::string const& msg = "Failed to estimate adaptive moments while measuring KRON flux";
            if (source) {
                LSST_EXCEPT_ADD(e, msg);
            } else {
                e = LSST_EXCEPT(lsst::pex::exceptions::NotFoundException, msg);
            }
            throw e;
        }
        Ixx = shapeImpl.getIxx();
        Ixy = shapeImpl.getIxy();
        Iyy = shapeImpl.getIyy();
    }
    /*
     * The shape is an ellipse that's axis-aligned in (u, v) [<uv> = 0] after rotation by theta:
     * <x^2> + <y^2> = <u^2> + <v^2>
     * <x^2> - <y^2> = cos(2 theta)*(<u^2> - <v^2>)
     * 2*<xy>        = sin(2 theta)*(<u^2> - <v^2>)
     */
    double const Iuu_p_Ivv = Ixx + Iyy;                             // <u^2> + <v^2>
    double const Iuu_m_Ivv = ::sqrt(::pow(Ixx - Iyy, 2) + 4*::pow(Ixy, 2)); // <u^2> - <v^2>
    double const Iuu = 0.5*(Iuu_p_Ivv + Iuu_m_Ivv);                         // (major axis)^2; a
    double const Ivv = 0.5*(Iuu_p_Ivv - Iuu_m_Ivv);                         // (minor axis)^2; b
    double const theta = 0.5*::atan2(2*Ixy, Ixx - Iyy);                     // angle of a +ve from x axis

    double const a = _nSigmaForRadius*::sqrt(Iuu);
    double const b = _nSigmaForRadius*::sqrt(Ivv);
    
    FootprintFindMoment<MaskedImageT, afwDetection::Psf::Image> iRFunctor(mimage, xcen, ycen, a/b, theta);
    // Build an elliptical Footprint of the proper size
    afwGeom::Point2I center(peak->getFx() + 0.5, peak->getFy() + 0.5);
#if HAVE_ellipticalFootprint
    PTR(afwDetection::Footprint) foot(ellipticalFootprint(center, a, b, theta));
                                 
    iRFunctor.apply(*foot);
#else
    afwDetection::Footprint foot(center, a, b, theta);
                                 
    iRFunctor.apply(foot);
#endif
    radius = iRFunctor.getIr();

    double const r2 = _nRadiusForFlux*radius; // radius to measure within

    try {
        std::pair<double, double> const fluxFluxErr =
            photometry::calculateSincApertureFlux(exposure->getMaskedImage(), peak->getFx(), peak->getFy(),
                                                  0.0, r2, theta, 1 - b/a);
        flux = fluxFluxErr.first;
        fluxErr = fluxFluxErr.second;
    } catch(pexExceptions::LengthErrorException &e) {
        LSST_EXCEPT_ADD(e, (boost::format(
                      "Measuring Kron flux for object at (%.3f, %.3f); aperture radius %g theta %g")
                            % peak->getFx() % peak->getFy()
                            % r2 % (theta*180/PI)).str());
        throw e;
    }

    return boost::make_shared<KronPhotometry>(radius, flux, fluxErr);
}

/*
 * Declare the existence of a "KRON" algorithm to MeasurePhotometry
 *
 * \cond
 */
#define MAKE_PHOTOMETRYS(TYPE)                                          \
    MeasurePhotometry<afwImage::Exposure<TYPE> >::declare("KRON", \
        &KronPhotometry::doMeasure<afwImage::Exposure<TYPE> >, \
        &KronPhotometry::doConfigure \
    )

namespace {
    volatile bool isInstance[] = {
        MAKE_PHOTOMETRYS(float)
#if 0
        ,MAKE_PHOTOMETRYS(double)
#endif
    };
}
    
// \endcond

}}}
