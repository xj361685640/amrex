//BL_COPYRIGHT_NOTICE

//
// $Id: LinOp.cpp,v 1.7 1998-07-07 17:25:15 lijewski Exp $
//

#ifdef BL_USE_NEW_HFILES
#include <cstdlib>
#else
#include <stdlib.h>
#endif

#include <ParmParse.H>
#include <ParallelDescriptor.H>

#include <LO_BCTYPES.H>
#include <LO_F.H>
#include <LinOp.H>

bool LinOp::initialized = false;
int LinOp::def_harmavg  = 0;
int LinOp::def_verbose  = 0;
int LinOp::def_maxorder = 2;

#ifndef NDEBUG
//
// LinOp::applyBC fills LinOp_grow ghost cells with data expected in
// LinOp::apply therefore, the incoming MultiFab to LinOp::applyBC better
// have this many ghost allocated.
//
const int LinOp_grow = 1;
#endif

void
LinOp::initialize ()
{
    ParmParse pp("Lp");

    pp.query("harmavg", def_harmavg);
    pp.query("verbose", def_verbose);
    pp.query("v", def_verbose);

    if (ParallelDescriptor::IOProcessor() && def_verbose)
    {
        cout << "def_harmavg = " << def_harmavg << '\n';
    }
    initialized = true;
}

LinOp::LinOp (const BndryData& _bgb,
              const Real       _h)
    :
    bgb(_bgb)
{
    Real __h[BL_SPACEDIM];
    for (int i = 0; i < BL_SPACEDIM; i++)
    {
        __h[i] = _h;
    }
    initConstruct(__h);
}

LinOp::LinOp (const BndryData& _bgb,
              const Real*      _h)
    :
    bgb(_bgb)
{
    initConstruct(_h);
}

LinOp::~LinOp ()
{
    for (int i = 0; i < maskvals.length(); ++i)
    {
        for (int j = 0; j < maskvals[i].length(); ++j)
        {
            for (int k = 0; k < maskvals[i][j].length(); ++k)
            {
                delete maskvals[i][j][k];
            }
        }
    }
}

LinOp::LinOp (const LinOp& _lp,
          int          level)
    :
    bgb(_lp.bgb)
{
    harmavg = _lp.harmavg;
    verbose = _lp.verbose;
    gbox.resize(1);
    gbox[0] = _lp.boxArray(level);
    geomarray.resize(1);
    geomarray[0] = bgb.getGeom();
    h.resize(1);
    assert(_lp.numLevels() > level);
    h[0] = _lp.h[level];
    undrrelxr.resize(1);
    undrrelxr[0] = _lp.undrrelxr[level];
}

void
LinOp::initConstruct (const Real* _h)
{
    if (!initialized)
        initialize();

    harmavg = def_harmavg;
    verbose = def_verbose;
    gbox.resize(1);
    const int level = 0;
    gbox[level] = bgb.boxes();
    geomarray.resize(1);
    geomarray[level] = bgb.getGeom();
    h.resize(1);
    maxorder = def_maxorder;

    for (int i = 0; i < BL_SPACEDIM; i++)
    {
        h[level][i] = _h[i];
    }
    undrrelxr.resize(1);
    undrrelxr[level] = new BndryRegister(gbox[level], 1, 0, 0, 1);
    maskvals.resize(1);
    maskvals[level].resize(gbox[level].length());
    //
    // For each orientation, build NULL masks, then use distributed allocation.
    //
    for (int i = 0; i < gbox[level].length(); i++)
    {
        maskvals[level][i].resize(2*BL_SPACEDIM, 0);
    }
    const int MyProc = ParallelDescriptor::MyProc();
    for (OrientationIter oitr; oitr; ++oitr)
    {
        Orientation face = oitr();
        const FabSet& bndry = bgb[face];
        for (int i = 0; i < gbox[level].length(); i++)
        {
            if (bndry.DistributionMap()[i] == MyProc)
            {
                const PArray<Mask>& pam = bgb.bndryMasks(face);
                assert(maskvals[level][i][face] == 0);
                maskvals[level][i][face] = new Mask(pam[i].box(), 1);
                maskvals[level][i][face]->copy(pam[i]);
            }
        }
    }
}

void
LinOp::apply (MultiFab&      out,
              MultiFab&      in,
              int            level,
              LinOp::BC_Mode bc_mode)
{
    applyBC(in,level,bc_mode);
    Fapply(out,in,level);
}

void
LinOp::applyBC (MultiFab&      inout,
                int            level,
                LinOp::BC_Mode bc_mode)
{
    //
    // The inout MultiFab needs at least LinOp_grow ghost cells for applyBC.
    //
    assert(inout.nGrow() >= LinOp_grow);
    //
    // The inout MultiFab must have at least Periodic_BC_grow cells for the
    // algorithms taking care of periodic boundary conditions.
    //
    assert(inout.nGrow() >= LinOp_grow);
    //
    // No coarsened boundary values, cannot apply inhomog at lev>0.
    //
    assert(!(level > 0 && bc_mode == Inhomogeneous_BC));

    int nc      = inout.nComp();    
    int flagden = 1; // Fill in undrrelxr.
    int flagbc  = 1; // Fill boundary data.

    if (bc_mode == LinOp::Homogeneous_BC)
        flagbc = 0; // No data if homog.
    //
    // Only single-component solves supported (verified) by this class.
    //
    assert(nc == 1);

    inout.FillBoundary();

    prepareForLevel(level);
    //
    // Do periodic fixup.
    //
    geomarray[level].FillPeriodicBoundary(inout, 0, nc);
    //
    // Fill boundary cells.
    //
    for (OrientationIter oitr; oitr; ++oitr)
    {
        const Array< Array<BoundCond> >& b = bgb.bndryConds(oitr());
        const Array<Real>& r               = bgb.bndryLocs(oitr());
        FabSet& f                          = (*undrrelxr[level])[oitr()];
        int cdr                            = oitr();
        const FabSet& fs                   = bgb.bndryValues(oitr());
        const int comp                     = 0;

        for (MultiFabIterator inoutmfi(inout); inoutmfi.isValid(false);
             ++inoutmfi)
        {
            DependentFabSetIterator ffsi(inoutmfi, f);
            DependentFabSetIterator fsfsi(inoutmfi, fs);

            const int gn = inoutmfi.index();

            assert(gbox[level][inoutmfi.index()] == inoutmfi.validbox());

            const Mask& m = *maskvals[level][gn][oitr()];
            Real bcl      = r[gn];
            int bct       = b[gn][comp];

            FORT_APPLYBC(&flagden, &flagbc, &maxorder,
                         inoutmfi().dataPtr(), 
                         ARLIM(inoutmfi().loVect()), ARLIM(inoutmfi().hiVect()),
                         &cdr, &bct, &bcl,
                         fsfsi().dataPtr(), 
                         ARLIM(fsfsi().loVect()), ARLIM(fsfsi().hiVect()),
                         m.dataPtr(),
                         ARLIM(m.loVect()), ARLIM(m.hiVect()),
                         ffsi().dataPtr(),
                         ARLIM(ffsi().loVect()), ARLIM(ffsi().hiVect()),
                         inoutmfi.validbox().loVect(),
                         inoutmfi.validbox().hiVect(), &nc, h[level]);
        }
    }
}
    
void
LinOp::residual (MultiFab&       residL,
                 const MultiFab& rhsL,
                 MultiFab&       solnL,
                 int             level,
                 LinOp::BC_Mode  bc_mode)
{
    apply(residL, solnL, level, bc_mode);

    for (MultiFabIterator solnLmfi(solnL); solnLmfi.isValid(false); ++solnLmfi)
    {
        DependentMultiFabIterator residLmfi(solnLmfi, residL);
        DependentMultiFabIterator rhsLmfi(solnLmfi, rhsL);
        int nc = residL.nComp();
        //
        // Only single-component solves supported (verified) by this class.
        //
        assert(nc == 1);
        assert(gbox[level][solnLmfi.index()] == solnLmfi.validbox());
        FORT_RESIDL(
            residLmfi().dataPtr(), 
            ARLIM(residLmfi().loVect()), ARLIM(residLmfi().hiVect()),
            rhsLmfi().dataPtr(), 
            ARLIM(rhsLmfi().loVect()), ARLIM(rhsLmfi().hiVect()),
            residLmfi().dataPtr(), 
            ARLIM(residLmfi().loVect()), ARLIM(residLmfi().hiVect()),
            solnLmfi.validbox().loVect(), solnLmfi.validbox().hiVect(), &nc);
    }
}

void
LinOp::smooth (MultiFab&       solnL,
               const MultiFab& rhsL,
               int             level,
               LinOp::BC_Mode  bc_mode)
{
    for (int redBlackFlag = 0; redBlackFlag < 2; redBlackFlag++)
    {
        applyBC(solnL, level, bc_mode);
        Fsmooth(solnL, rhsL, level, redBlackFlag);
    }
}

Real
LinOp::norm (const MultiFab& in,
             int             level) const
{
    Real norm = 0.0;
    for (ConstMultiFabIterator inmfi(in); inmfi.isValid(false); ++inmfi)
    {
        Real tnorm = inmfi().norm(gbox[level][inmfi.index()]);
        norm      += tnorm*tnorm;
    }
    ParallelDescriptor::ReduceRealSum(norm);
    return norm;
}

void
LinOp::prepareForLevel (int level)
{
    if (level == 0)
        return;

    LinOp::prepareForLevel(level-1);

    if (h.size() > level)
        return;
    //
    // Assume from here down that this is a new level one coarser than existing
    //
    assert(h.size() == level);
    h.resize(level+1);
    for (int i = 0; i < BL_SPACEDIM; ++i)
    {
        h[level][i] = h[level-1][i]*2.0;
    }
    geomarray.resize(level+1);
    geomarray[level].define(::coarsen(geomarray[level-1].Domain(),2));
    const Box& curdomain = geomarray[level].Domain();
    //
    // Add a box to the new coarser level (assign removes old BoxArray).
    //
    gbox.resize(level+1);
    gbox[level] = gbox[level-1];
    gbox[level].coarsen(2);
    //
    // Add the BndryRegister of relax values to the new coarser level.
    //
    assert(undrrelxr.size() == level);
    undrrelxr.resize(level+1);
    undrrelxr[level] = new BndryRegister(gbox[level], 1, 0, 0, 1);
    //
    // Add an Array of Array of maskvals to the new coarser level
    // For each orientation, build NULL masks, then use distributed allocation
    // Initial masks for coarse levels, ignore outside_domain possibility since
    // we always solve homogeneous equation on coarse levels.
    //
    assert(maskvals.length() == level);
    maskvals.resize(level+1);
    maskvals[level].resize(gbox[level].length());
    for (int i = 0; i < gbox[level].length(); i++)
    {
        maskvals[level][i].resize(2*BL_SPACEDIM, (Mask*)0);
    }
    Array<IntVect> pshifts(27);

    const int MyProc = ParallelDescriptor::MyProc();

    for (OrientationIter oitr; oitr; ++oitr)
    {
        Orientation face = oitr();
        //
        // Use bgb's distribution map for masks.
        //
        const FabSet& bndry = bgb[face];

        for (ConstFabSetIterator bndryfsi(bgb[face]); bndryfsi.isValid(false);
             ++bndryfsi)
        {
            int gn = bndryfsi.index();
            Box bx_k = ::adjCell(gbox[level][gn], face, 1);
            assert(maskvals[level][gn][face] == 0);
            maskvals[level][gn][face] = new Mask(bx_k, 1);
            Mask& curmask = *(maskvals[level][gn][face]);
            curmask.setVal(BndryData::not_covered);
            for (int gno = 0; gno < gbox[level].length(); ++gno)
            {
                if (gno != gn  &&  bx_k.intersects(gbox[level][gno]))
                {
                    Box btmp = gbox[level][gno] & bx_k;
                    curmask.setVal(BndryData::covered, btmp,0);
                }
            }
            //
            // Now take care of periodic wraparounds.
            //
            Geometry& curgeom = geomarray[level];

            if (curgeom.isAnyPeriodic() && !curdomain.contains(bx_k))
            {
                curgeom.periodicShift(curdomain, bx_k, pshifts);

                for (int iiv = 0; iiv < pshifts.length(); iiv++)
                {
                    curmask.shift(pshifts[iiv]);
                    for (int gno = 0; gno < gbox[level].length(); ++gno)
                    {
                        Box btmp = gbox[level][gno] & curmask.box();
                        curmask.setVal(BndryData::covered, btmp,0);
                    }
                    curmask.shift(-pshifts[iiv]);
                }
            }
        }
    }
}

void
LinOp::makeCoefficients (MultiFab&       cs,
                         const MultiFab& fn,
                         int             level)
{
    int nc = 1;
    //
    // Determine index type of incoming MultiFab.
    //
    const IndexType iType(fn.boxArray()[0].ixType());
    const IndexType cType(D_DECL(IndexType::CELL, IndexType::CELL, IndexType::CELL));
    const IndexType xType(D_DECL(IndexType::NODE, IndexType::CELL, IndexType::CELL));
    const IndexType yType(D_DECL(IndexType::CELL, IndexType::NODE, IndexType::CELL));
#if (BL_SPACEDIM == 3)    
    const IndexType zType(D_DECL(IndexType::CELL, IndexType::CELL, IndexType::NODE));
#endif

    int cdir;
    if (iType == cType)
    {
        cdir = -1;
    }
    else if (iType == xType)
    {
        cdir = 0;
    }
    else if (iType == yType)
    {
        cdir = 1;
#if (BL_SPACEDIM == 3)
    }
    else if (iType == zType)
    {
        cdir = 2;
#endif    
    }
    else
    {
        BoxLib::Error("LinOp::makeCoeffients: Bad index type");
    }

    BoxArray d(gbox[level]);
    if (cdir >= 0)
        d.surroundingNodes(cdir);
    //
    // Only single-component solves supported (verified) by this class.
    //
    const int nComp=1;
    const int nGrow=0;
    cs.define(d, nComp, nGrow, Fab_allocate);

    const BoxArray& grids = gbox[level];

    MultiFabIterator csmfi(cs);

    switch (cdir)
    {
    case -1:
        for ( ; csmfi.isValid(false); ++csmfi)
        {
            DependentMultiFabIterator fnmfi(csmfi, fn);

            FORT_AVERAGECC(csmfi().dataPtr(), ARLIM(csmfi().loVect()),
                           ARLIM(csmfi().hiVect()),fnmfi().dataPtr(),
                           ARLIM(fnmfi().loVect()),ARLIM(fnmfi().hiVect()),
                           grids[csmfi.index()].loVect(),
                           grids[csmfi.index()].hiVect(), &nc);
        }
        break;
    case 0:
    case 1:
    case 2:
        if (harmavg)
        {
            for ( ; csmfi.isValid(false); ++csmfi)
            {
                DependentMultiFabIterator fnmfi(csmfi, fn);

                FORT_HARMONIC_AVERAGEEC(csmfi().dataPtr(),
                                        ARLIM(csmfi().loVect()),
                                        ARLIM(csmfi().hiVect()),
                                        fnmfi().dataPtr(),
                                        ARLIM(fnmfi().loVect()),
                                        ARLIM(fnmfi().hiVect()),
                                        grids[csmfi.index()].loVect(),
                                        grids[csmfi.index()].hiVect(),
                                        &nc,&cdir);
            }
        }
        else
        {
            for ( ; csmfi.isValid(false); ++csmfi)
            {
                DependentMultiFabIterator fnmfi(csmfi, fn);

                FORT_AVERAGEEC(csmfi().dataPtr(),ARLIM(csmfi().loVect()),
                               ARLIM(csmfi().hiVect()),fnmfi().dataPtr(), 
                               ARLIM(fnmfi().loVect()),ARLIM(fnmfi().hiVect()),
                               grids[csmfi.index()].loVect(),
                               grids[csmfi.index()].hiVect(),&nc, &cdir);
            }
        }
        break;
    default:
        BoxLib::Error("LinOp:: bad coefficient coarsening direction!");
    }
}

ostream&
operator<< (ostream&     os,
            const LinOp& lp)
{
    if (ParallelDescriptor::IOProcessor())
    {
        os << "LinOp" << '\n';
        os << "Grids: " << '\n';
        for (int level = 0; level < lp.h.size(); ++level)
        {
            os << " level = " << level << ": " << lp.gbox[level] << '\n';
        }
        os << "Grid Spacing: " << '\n';
        for (int level = 0; level < lp.h.size(); ++level)
        {
            os << " level = " << level << ", dx = ";
            for (int d =0; d < BL_SPACEDIM; ++d)
            {
                os << lp.h[level][d] << "  ";
            }
            os << '\n';
        }
        os << "Harmonic average? " << (lp.harmavg == 1 ? "yes" : "no") << '\n';
        os << "Verbosity: " << lp.verbose << '\n';
        os << "Max Order: " << lp.maxorder << '\n';
    }

    if (ParallelDescriptor::IOProcessor())
    {
        os << "Masks:" << '\n';
    }
    for (int level = 0; level < lp.h.size(); ++level)
    {
        if (ParallelDescriptor::IOProcessor())
            os << "level = " << level << '\n';
        ParallelDescriptor::Synchronize();
        for (int nproc = 0; nproc < ParallelDescriptor::NProcs(); ++nproc)
        {
            if (nproc == ParallelDescriptor::MyProc())
            {
                os << "Processor " << nproc << '\n';
                for (OrientationIter oitr; oitr; ++oitr)
                {
                    Orientation face = oitr();
                    for (int i=0; i<lp.boxArray().length(); ++i)
                    {
                        if (lp.maskvals[level][i][face])
                        {
                            os << *lp.maskvals[level][i][face];
                        }
                    }
                }
            }
            ParallelDescriptor::Synchronize();
        }
    }
    
    return os;
}

