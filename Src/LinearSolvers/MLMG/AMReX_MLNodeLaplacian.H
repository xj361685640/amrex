#ifndef AMREX_ML_NODE_LAPLACIAN_H_
#define AMREX_ML_NODE_LAPLACIAN_H_

#include <AMReX_MLNodeLinOp.H>

namespace amrex {

// del dot (sigma grah phi) = rhs
// where phi and rhs are nodal, and sigma is cell-centered.

class MLNodeLaplacian
    : public MLNodeLinOp
{
public :

    enum struct CoarseningStrategy { Sigma, RAP };

    MLNodeLaplacian () {}
    MLNodeLaplacian (const Vector<Geometry>& a_geom,
                     const Vector<BoxArray>& a_grids,
                     const Vector<DistributionMapping>& a_dmap,
                     const LPInfo& a_info = LPInfo(),
                     const Vector<FabFactory<FArrayBox> const*>& a_factory = {});
#ifdef AMREX_USE_EB
    MLNodeLaplacian (const Vector<Geometry>& a_geom,
                     const Vector<BoxArray>& a_grids,
                     const Vector<DistributionMapping>& a_dmap,
                     const LPInfo& a_info,
                     const Vector<EBFArrayBoxFactory const*>& a_factory);
#endif
    virtual ~MLNodeLaplacian ();

    MLNodeLaplacian (const MLNodeLaplacian&) = delete;
    MLNodeLaplacian (MLNodeLaplacian&&) = delete;
    MLNodeLaplacian& operator= (const MLNodeLaplacian&) = delete;
    MLNodeLaplacian& operator= (MLNodeLaplacian&&) = delete;

    void define (const Vector<Geometry>& a_geom,
                 const Vector<BoxArray>& a_grids,
                 const Vector<DistributionMapping>& a_dmap,
                 const LPInfo& a_info = LPInfo(),
                 const Vector<FabFactory<FArrayBox> const*>& a_factory = {});

    void setRZCorrection (bool rz) { m_is_rz = rz; }

    void setSigma (int amrlev, const MultiFab& a_sigma);

    void compRHS (const Vector<MultiFab*>& rhs, const Vector<MultiFab*>& vel,
                  const Vector<const MultiFab*>& rhnd,
                  const Vector<MultiFab*>& rhcc);

    void updateVelocity (const Vector<MultiFab*>& vel, const Vector<MultiFab const*>& sol) const;

    void compSyncResidualCoarse (MultiFab& sync_resid, const MultiFab& phi,
                                 const MultiFab& vold, const MultiFab* rhcc,
                                 const BoxArray& fine_grids, const IntVect& ref_ratio);

    void compSyncResidualFine (MultiFab& sync_resid, const MultiFab& phi, const MultiFab& vold,
                               const MultiFab* rhcc);

    void setGaussSeidel (bool flag) { m_use_gauss_seidel = flag; }
    void setHarmonicAverage (bool flag) { m_use_harmonic_average = flag; }
    void setSimpleInterpolation (bool flag) { m_use_simple_interp = flag; } // for P in RAP

#ifndef AMREX_USE_EB
    void setCoarseningStrategy (CoarseningStrategy cs) { m_coarsening_strategy = cs; }
#endif

protected:

    virtual void restriction (int amrlev, int cmglev, MultiFab& crse, MultiFab& fine) const final;
    virtual void interpolation (int amrlev, int fmglev, MultiFab& fine, const MultiFab& crse) const final;
    virtual void averageDownSolutionRHS (int camrlev, MultiFab& crse_sol, MultiFab& crse_rhs,
                                         const MultiFab& fine_sol, const MultiFab& fine_rhs) final;

    virtual void reflux (int crse_amrlev,
                         MultiFab& res, const MultiFab& crse_sol, const MultiFab& crse_rhs,
                         MultiFab& fine_res, MultiFab& fine_sol, const MultiFab& fine_rhs) const final;

    virtual void prepareForSolve () final;
    virtual bool isSingular (int amrlev) const final { return false; }
    virtual bool isBottomSingular () const final { return m_is_bottom_singular; }
    virtual void applyBC (int amrlev, int mglev, MultiFab& phi, BCMode bc_mode,
                          bool skip_fillboundary=false) const final;
    virtual void Fapply (int amrlev, int mglev, MultiFab& out, const MultiFab& in) const final;
    virtual void Fsmooth (int amrlev, int mglev, MultiFab& sol, const MultiFab& rhs) const final;
    virtual void normalize (int amrlev, int mglev, MultiFab& mf) const final;

    virtual Real getAScalar () const final { return  0.0; }
    virtual Real getBScalar () const final { return -1.0; }
    virtual MultiFab const* getACoeffs (int amrlev, int mglev) const final { return nullptr; }
    virtual std::array<MultiFab const*,AMREX_SPACEDIM> getBCoeffs (int amrlev, int mglev) const final
        { return {AMREX_D_DECL(m_sigma[amrlev][mglev][0].get(),
                               m_sigma[amrlev][mglev][2].get(),
                               m_sigma[amrlev][mglev][3].get())}; }

    virtual void fixUpResidualMask (int amrlev, iMultiFab& resmsk) final;

private:

    int m_is_rz = 0;

#ifdef AMREX_USE_EB
    CoarseningStrategy m_coarsening_strategy = CoarseningStrategy::RAP;
#else
    CoarseningStrategy m_coarsening_strategy = CoarseningStrategy::Sigma;
#endif

    Vector<Vector<std::array<std::unique_ptr<MultiFab>,AMREX_SPACEDIM> > > m_sigma;
    Vector<Vector<std::unique_ptr<MultiFab> > > m_stencil;

#ifdef AMREX_USE_EB
    // they could be MultiCutFab
    Vector<std::unique_ptr<MultiFab> > m_connection;
    Vector<std::unique_ptr<MultiFab> > m_integral;
    bool m_connection_built = false;
#endif

    bool m_use_gauss_seidel = true;
    bool m_use_harmonic_average = false;
    bool m_use_simple_interp = false;

    bool m_is_bottom_singular = false;
    bool m_masks_built = false;
    //
    // functions
    //
    void averageDownCoeffs ();
    void averageDownCoeffsToCoarseAmrLevel (int flev);
    void averageDownCoeffsSameAmrLevel (int amrlev);

    void FillBoundaryCoeff (MultiFab& sigma, const Geometry& geom);

    void buildMasks ();

    void buildStencil ();

#ifdef AMREX_USE_EB
    void buildConnection ();
#endif
};

}

#endif
