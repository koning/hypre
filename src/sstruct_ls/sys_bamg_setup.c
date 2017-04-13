/*BHEADER**********************************************************************
 * Copyright (c) 2008,  Lawrence Livermore National Security, LLC.
 * Produced at the Lawrence Livermore National Laboratory.
 * This file is part of HYPRE.  See file COPYRIGHT for details.
 *
 * HYPRE is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (as published by the Free
 * Software Foundation) version 2.1 dated February 1999.
 *
 * $Revision$
 ***********************************************************************EHEADER*/

#include "_hypre_sstruct_ls.h"
#include "sys_bamg.h"

#define hypre_BAMGSetCIndex(cdir, cindex)   \
{                                           \
  hypre_SetIndex( (cindex), 0 );            \
  hypre_IndexD(cindex, cdir) = 0;           \
}

#define hypre_BAMGSetFIndex(cdir, findex)   \
{                                           \
  hypre_SetIndex( (findex), 0 );            \
  hypre_IndexD(findex, cdir) = 1;           \
}

#define hypre_BAMGSetStride(cdir, stride)   \
{                                           \
  hypre_SetIndex( (stride), 1 );            \
  hypre_IndexD(stride, cdir) = 2;           \
}

HYPRE_Int hypre_SysBAMGSetupGrids(
  hypre_SysBAMGData* data,
  hypre_SStructPMatrix* A,
  HYPRE_Real* relax_weights,
  HYPRE_Int* cmaxsize
);

HYPRE_Int hypre_SysBAMGSetupMV(
  hypre_SysBAMGData* data,
  hypre_SStructPMatrix* A,
  hypre_SStructPVector* b,
  hypre_SStructPVector* x
);

HYPRE_Int hypre_SysBAMGSetupTV(
  hypre_SysBAMGData* data,
  hypre_SStructPVector*** tv,
  HYPRE_Real* relax_weights
);

HYPRE_Int hypre_SysBAMGSetupOperators(
  hypre_SysBAMGData* data,
  hypre_SStructPVector*** tv,
  HYPRE_Int num_tv_,
  HYPRE_Real* relax_weights,
  HYPRE_Int cmaxsize
);

HYPRE_Int hypre_SysBAMGCoarsen(
  hypre_SStructPGrid* finePGrid,
  hypre_Index index,
  hypre_Index stride,
  HYPRE_Int prune,
  hypre_SStructPGrid** coarsePGrid_ptr
);


/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGSetup
(
  void*                   sys_bamg_vdata,
  hypre_SStructMatrix*    A_in,
  hypre_SStructVector*    b_in,
  hypre_SStructVector*    x_in
)
{
  hypre_SysBAMGData*      data              = (hypre_SysBAMGData *)sys_bamg_vdata;

  MPI_Comm                comm              = (data->comm);

  HYPRE_Int               num_refine        = (data->num_refine);
  HYPRE_Int               num_rtv           = (data->num_rtv);
  HYPRE_Int               num_stv           = (data->num_stv);

  HYPRE_Int               num_tv            = num_rtv + num_stv;

  HYPRE_Int               symmetric;
  HYPRE_Int               nsym;

  hypre_SStructPMatrix*   A;
  hypre_SStructPVector*   b;
  hypre_SStructPVector*   x;

  HYPRE_Int               max_iter;
  HYPRE_Int               max_levels;
  HYPRE_Int               num_levels;

  HYPRE_Int*              cdir_l;
  HYPRE_Int*              active_l;
  hypre_SStructPGrid**    PGrid_l;
  hypre_SStructPGrid**    P_PGrid_l;

  hypre_SStructPMatrix**  A_l;
  hypre_SStructPMatrix**  P_l;
  hypre_SStructPMatrix**  RT_l;
  hypre_SStructPVector**  b_l;
  hypre_SStructPVector**  x_l;

  hypre_SStructPVector**  tx_l;
  hypre_SStructPVector**  r_l;
  hypre_SStructPVector**  e_l;

  void**                  relax_data_l;
  void**                  matvec_data_l;
  void**                  restrict_data_l;
  void**                  interp_data_l;

  HYPRE_Real*             relax_weights;

  HYPRE_Int               ndim;

  HYPRE_Int               cmaxsize;
  HYPRE_Int               i, k, l, d;

  hypre_SStructPVector*** tv;     // tv[l][k] == k'th test vector on level l

#if DEBUG_SYSBAMG > 1
  char                    filename[255];
#endif

  /*----------------------------------------------------------------------------------------------
   * Refs to A,x,b (the PMatrix & PVectors within the input SStructMatrix & SStructVectors)
   *  -- ignore parts != 0 XXX
   *----------------------------------------------------------------------------------------------*/

  hypre_SStructPMatrixRef(hypre_SStructMatrixPMatrix(A_in, 0), &A);
  hypre_SStructPVectorRef(hypre_SStructVectorPVector(b_in, 0), &b);
  hypre_SStructPVectorRef(hypre_SStructVectorPVector(x_in, 0), &x);

  /*----------------------------------------------------------------------------------------------
   * Compute max_levels value based on the grid
   *----------------------------------------------------------------------------------------------*/
  {
    hypre_SStructPGrid* PGrid = hypre_SStructPMatrixPGrid(A);
    hypre_StructGrid*   SGrid = hypre_SStructPGridSGrid(PGrid, 0);

    ndim = hypre_StructGridNDim(SGrid);

    hypre_Box* cbox = hypre_BoxDuplicate(hypre_StructGridBoundingBox(SGrid));

   max_levels = 0;
   for ( d = 0; d < ndim; d++ ) max_levels += hypre_Log2(hypre_BoxSizeD(cbox, d)) + 2;
   if ((data->max_levels) > 0)
      max_levels = hypre_min(max_levels, (data->max_levels));
   (data->max_levels) = max_levels;

  /*----------------------------------------------------------------------------------------------
   * Allocate arrays
   *----------------------------------------------------------------------------------------------*/

    PGrid_l   = (data->PGrid_l)   = hypre_TAlloc(hypre_SStructPGrid*, max_levels);
    P_PGrid_l = (data->P_PGrid_l) = hypre_TAlloc(hypre_SStructPGrid*, max_levels);
    cdir_l    = (data->cdir_l)    = hypre_TAlloc(HYPRE_Int, max_levels);
    active_l  = (data->active_l)  = hypre_TAlloc(HYPRE_Int, max_levels);

    (data->PGrid_l)   = PGrid_l;
    (data->P_PGrid_l) = P_PGrid_l;
    (data->cdir_l)    = cdir_l;
    (data->active_l)  = active_l;

    PGrid_l[0]     = PGrid;
    P_PGrid_l[0]   = NULL;

    relax_weights = hypre_CTAlloc(HYPRE_Real, max_levels);
  }

  /*----------------------------------------------------------------------------------------------
   * Set up coarse grids
   *----------------------------------------------------------------------------------------------*/

  hypre_SysBAMGSetupGrids( data, A, relax_weights, &cmaxsize );

  num_levels = (data->num_levels);

  /*----------------------------------------------------------------------------------------------
   * Allocate/Create/Assemble matrix and vector structures
   *----------------------------------------------------------------------------------------------*/

  A_l  = hypre_TAlloc(hypre_SStructPMatrix*, num_levels);
  P_l  = hypre_TAlloc(hypre_SStructPMatrix*, num_levels - 1);
  RT_l = hypre_TAlloc(hypre_SStructPMatrix*, num_levels - 1);
  b_l  = hypre_TAlloc(hypre_SStructPVector*, num_levels);
  x_l  = hypre_TAlloc(hypre_SStructPVector*, num_levels);
  tx_l = hypre_TAlloc(hypre_SStructPVector*, num_levels);
  r_l  = tx_l;
  e_l  = tx_l;

  (data->A_l)  = A_l;
  (data->P_l)  = P_l;
  (data->RT_l) = RT_l;
  (data->b_l)  = b_l;
  (data->x_l)  = x_l;
  (data->tx_l) = tx_l;
  (data->r_l)  = r_l;
  (data->e_l)  = e_l;

  hypre_SysBAMGSetupMV( data, A, b, x );

  /*----------------------------------------------------------------------------------------------
   * Allocate/Create auxiliary data structures
   *----------------------------------------------------------------------------------------------*/

  relax_data_l    = (data->relax_data_l)    = hypre_TAlloc(void *, num_levels);
  matvec_data_l   = (data->matvec_data_l)   = hypre_TAlloc(void *, num_levels);
  restrict_data_l = (data->restrict_data_l) = hypre_TAlloc(void *, num_levels);
  interp_data_l   = (data->interp_data_l)   = hypre_TAlloc(void *, num_levels);

  for (l = 0; l < num_levels; l++)          relax_data_l[l] = hypre_SysBAMGRelaxCreate(comm);
  for (l = 0; l < num_levels; l++)          hypre_SStructPMatvecCreate(&matvec_data_l[l]);
  for (l = 0; l < (num_levels - 1); l++)    hypre_SysSemiRestrictCreate(&restrict_data_l[l]);
  for (l = 0; l < (num_levels - 1); l++)    hypre_SysSemiInterpCreate(&interp_data_l[l]);

  /*----------------------------------------------------------------------------------------------
   * Create/Assemble test vectors, set values of initial, random tv's
   *----------------------------------------------------------------------------------------------*/

  // XXX assume 'symmetric' same for all vars
  symmetric = (data->symmetric) = hypre_SStructPMatrixSymmetric(A)[0][0];

  // XXX terrible hack to get test working
  symmetric = (data->symmetric) = 1;

  nsym      = ( symmetric ? 1 : 2 );

  sysbamg_ifdbg
  {
     hypre_printf("num_tv = %d = %d + %d; nsym = %d\n", num_tv, num_rtv, num_stv, nsym);
     fflush(stdout);
  }

  tv = hypre_TAlloc(hypre_SStructPVector**, num_levels);

  hypre_SysBAMGSetupTV( data, tv, relax_weights );

  /*----------------------------------------------------------------------------------------------
   * Set up operators (P_l, RT_l, A_l)
   *----------------------------------------------------------------------------------------------*/

  sysbamg_ifdbg
  {
     hypre_printf("Set up multigrid operators (num_levels=%d) ...\n", num_levels);
     fflush(stdout);
  }

  hypre_SysBAMGSetupOperators( data, tv, num_rtv*nsym, relax_weights, cmaxsize );

#if DEBUG_SYSBAMG_PFMG == 0
  /*----------------------------------------------------------------------------------------------
   * Refinement loop
   *----------------------------------------------------------------------------------------------*/

  for ( i = 0; i < num_refine; i++ )
  {
    /*--------------------------------------------------------------------------------------------
     * Compute the coarse-grid singular vectors and then prolongate them to the fine grid
     *--------------------------------------------------------------------------------------------*/

    sysbamg_ifdbg
    {
       hypre_printf("Compute singular vectors num_stv=%d ...\n", num_stv);
       fflush(stdout);
    }

    hypre_SysBAMGComputeSVecs( A_l[num_levels-1], num_stv, &(tv[num_levels-1][num_rtv*nsym]) );

    for ( k = num_rtv*nsym; k < num_tv*nsym; k++ ) {
      for ( l = num_levels - 2; l >= 0; l-- ) {
        hypre_SysSemiInterp( interp_data_l[l], P_l[l], tv[l+1][k], tv[l][k] );
      }
    }

    /*--------------------------------------------------------------------------------------------
     * Refine operators using coarse-grid singular vectors (P_l, RT_l, A_l)
     *--------------------------------------------------------------------------------------------*/

    sysbamg_ifdbg
    {
       hypre_printf("Refine multigrid operators (num_levels=%d) ...\n", num_levels);
       fflush(stdout);
    }

    hypre_SysBAMGSetupOperators( data, tv, num_tv*nsym, relax_weights, cmaxsize );
  }
#endif

  /*----------------------------------------------------------------------------------------------
   * Allocate space for log info
   *----------------------------------------------------------------------------------------------*/

  if ((data->logging) > 0) {
    max_iter = (data->max_iter);
    (data->norms)     = hypre_TAlloc(HYPRE_Real, max_iter);
    (data->rel_norms) = hypre_TAlloc(HYPRE_Real, max_iter);
  }

#if DEBUG_SYSBAMG > 1
  for (l = 0; l < (num_levels - 1); l++) {
    hypre_sprintf(filename, "sysbamg_A.%02d", l);
    hypre_SStructPMatrixPrint(filename, A_l[l], 0);
    hypre_sprintf(filename, "sysbamg_P.%02d", l);
    hypre_SStructPMatrixPrint(filename, P_l[l], 0);
  }
  hypre_sprintf(filename, "sysbamg_A.%02d", l);
  hypre_SStructPMatrixPrint(filename, A_l[l], 0);
#endif

  /*----------------------------------------------------------------------------------------------
   * Destroy Refs to A,x,b (the PMatrix & PVectors within
   * the input SStructMatrix & SStructVectors).
   *----------------------------------------------------------------------------------------------*/

  hypre_SStructPMatrixDestroy(A);
  hypre_SStructPVectorDestroy(x);
  hypre_SStructPVectorDestroy(b);

  for ( l = 0; l < num_levels; l++ ) {
    for ( k = 0; k < num_tv*nsym; k++ ) {
      hypre_SStructPVectorDestroy(tv[l][k]);
    }
    hypre_TFree(tv[l]);
  }
  hypre_TFree(tv);

  hypre_TFree(relax_weights);

  return hypre_error_flag;
}

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGSetupGrids
(
  hypre_SysBAMGData*      data,
  hypre_SStructPMatrix*   A,
  HYPRE_Real*             relax_weights,
  HYPRE_Int*              cmaxsize
)
{
  hypre_SStructPGrid**    PGrid_l           = (data->PGrid_l);
  hypre_SStructPGrid**    P_PGrid_l         = (data->P_PGrid_l);
  HYPRE_Int*              cdir_l            = (data->cdir_l);
  HYPRE_Int*              active_l          = (data->active_l);
  HYPRE_Int               max_levels        = (data->max_levels);
  HYPRE_Int               skip_relax        = (data->skip_relax);

  HYPRE_Int               ndim;
  HYPRE_Int               ndimCoarsen;
  hypre_Box*              cbox;

  HYPRE_Int               num_levels;
  HYPRE_Int               cdir;
  HYPRE_Int               periodic;
  HYPRE_Int               l, d;

  HYPRE_Int               dimSize;
  hypre_Index             findex;
  hypre_Index             cindex;
  hypre_Index             stride;

  hypre_SStructPGrid*     PGrid;
  hypre_StructGrid*       SGrid;


  PGrid = hypre_SStructPMatrixPGrid(A);
  SGrid = hypre_SStructPGridSGrid(PGrid, 0);

  ndim        = hypre_StructGridNDim(SGrid);
  ndimCoarsen = ndim;                           // XXX should be a parameter?

  cbox = hypre_BoxDuplicate(hypre_StructGridBoundingBox(SGrid));

#if DEBUG_SYSBAMG_PFMG
  hypre_printf("DEBUG_SYSBAMG_PFMG\n");

  /* compute PFMG dxyz */

  HYPRE_Real*             dxyz      = (data->dxyz);
  HYPRE_Int               dxyz_flag = 0;
  HYPRE_Real              min_dxyz;
  HYPRE_Real**            sys_dxyz;
  HYPRE_Real              alpha, beta;
  HYPRE_Real*             mean;
  HYPRE_Real*             deviation;
  hypre_Index             coarsen;
  HYPRE_Int               nvars, i;

  nvars    = hypre_SStructPMatrixNVars(A);

  sys_dxyz = hypre_TAlloc(HYPRE_Real*, nvars);

  for ( i = 0; i < nvars; i++)
    sys_dxyz[i] = hypre_TAlloc(HYPRE_Real, ndim);

  HYPRE_Int dxyz_zeroes = 0;
  for ( d = 0; d < ndim; d++ ) dxyz_zeroes += ( dxyz[d] == 0 );
  if (dxyz_zeroes != 0)
  {
    mean = hypre_CTAlloc(HYPRE_Real, ndim);
    deviation = hypre_CTAlloc(HYPRE_Real, ndim);

    dxyz_flag = 0;
    for (i = 0; i < nvars; i++)
    {
      hypre_PFMGComputeDxyz(hypre_SStructPMatrixSMatrix(A,i,i), sys_dxyz[i], mean, deviation);

      /* check if any var has a large (square) coeff. of variation */
      if (!dxyz_flag)
      {
        for (d = 0; d < ndim; d++)
        {
          /* square of coeff. of variation */
          deviation[d] -= mean[d]*mean[d];
          if (deviation[d]/(mean[d]*mean[d]) > .1) {
            dxyz_flag = 1;
            break;
          }
        }
      }

      for (d = 0; d < ndim; d++) {
        dxyz[d] += sys_dxyz[i][d];
      }
    }

    hypre_TFree(mean);
    hypre_TFree(deviation);
  }

  hypre_SetIndex(coarsen, 1); /* forces relaxation on finest grid */
#endif

  for (l = 0; l < max_levels; l++)
  {
    sysbamg_ifdbg{hypre_printf("%s l=%d\n", __func__, l); fflush(stdout);}

#if DEBUG_SYSBAMG_PFMG
    /* determine cdir */
    min_dxyz = 1;
    for ( d = 0; d < ndim; d++ ) min_dxyz += dxyz[d];
    cdir = -1;
    alpha = 0.0;
    for (d = 0; d < ndim; d++)
    {
      if ((hypre_BoxIMaxD(cbox, d) > hypre_BoxIMinD(cbox, d)) && (dxyz[d] < min_dxyz)) {
        min_dxyz = dxyz[d];
        cdir = d;
      }
      alpha += 1.0/(dxyz[d]*dxyz[d]);
    }
    relax_weights[l] = 2.0/3.0;

    /* If it's possible to coarsen, change relax_weights */
    beta = 0.0;
    if (cdir != -1) {
      if (dxyz_flag) {
        relax_weights[l] = 2.0/3.0;
      }
      else {
        for (d = 0; d < ndim; d++) {
          if (d != cdir) {
            beta += 1.0/(dxyz[d]*dxyz[d]);
          }
        }
        if (beta == alpha) {
          alpha = 0.0;
        }
        else {
          alpha = beta/alpha;
        }

        /* determine level Jacobi weights */
        if (ndim > 1) {
          relax_weights[l] = 2.0/(3.0 - alpha);
        }
        else {
          relax_weights[l] = 2.0/3.0; /* always 2/3 for 1-d */
        }
      }
    }
#else
    cdir = l % ndimCoarsen;
    relax_weights[l] = 2.0/3.0;

    // stop coarsening if lengths of dims to coarsen are not *all* multiples of 2
    // XXX should the min size be 2, 4, 8, ...?
    if ( cdir == 0 ) {
      for ( d = 0; d < ndimCoarsen; d++ ) {
        dimSize = hypre_BoxIMaxD(cbox,d) - hypre_BoxIMinD(cbox,d) + 1;
        if ( dimSize <= 2 || dimSize % 2 != 0 ) {
          cdir = -1;
        }
      }
    }
#endif

    if (cdir != -1) {
      /* don't coarsen if the cdir is periodic and not divisible by 2 */
      periodic = hypre_IndexD(hypre_StructGridPeriodic(PGrid_l[l]), cdir);
      hypre_printf("level %d  periodic[%d] = %d\n", l, cdir, periodic);
      if ((periodic) && (periodic % 2)) {
        cdir = -1;
      }

      /* don't coarsen if we've reached max_levels */
      if (l == (max_levels - 1)) {
        cdir = -1;
      }
    }

    /* stop coarsening */
    if (cdir == -1) {
      active_l[l] = 1; /* forces relaxation on coarsest grid */
      *cmaxsize = 0;
      for (d = 0; d < ndim; d++)
        *cmaxsize = hypre_max(*cmaxsize, hypre_BoxSizeD(cbox, d));
      hypre_printf("stop coarsening: l = %d\n", l);
      break;
    }

    sysbamg_ifdbg
    {
       hypre_printf( "l %d  cdir %d  Min %d Max %d\n",
                     l, cdir, hypre_BoxIMinD(cbox,cdir), hypre_BoxIMaxD(cbox,cdir) );
       fflush(stdout);
    }

    cdir_l[l] = cdir;

#if DEBUG_SYSBAMG_PFMG
    /* only relax @ level l if grid is already set to be coarsened in cdir since last relaxation */
    if (hypre_IndexD(coarsen, cdir) != 0) {
      active_l[l] = 1;
      hypre_SetIndex(coarsen, 0);
      hypre_IndexD(coarsen, cdir) = 1;
    }
    else {
      active_l[l] = 0;
      hypre_IndexD(coarsen, cdir) = 1;
    }
#else
    active_l[l] = ( cdir == 0 ? 1 : 0 );
#endif

    /* set cindex, findex, and stride */
    hypre_BAMGSetCIndex(cdir, cindex);
    hypre_BAMGSetFIndex(cdir, findex);
    hypre_BAMGSetStride(cdir, stride);

    /* coarsen cbox*/
    hypre_ProjectBox(cbox, cindex, stride);
    hypre_StructMapFineToCoarse(hypre_BoxIMin(cbox), cindex, stride, hypre_BoxIMin(cbox));
    hypre_StructMapFineToCoarse(hypre_BoxIMax(cbox), cindex, stride, hypre_BoxIMax(cbox));

    sysbamg_ifdbg{hypre_printf( "cbox Min and Max:\n" ); fflush(stdout);}
    hypre_PrintIndex( hypre_BoxIMin(cbox), ndim );
    hypre_PrintIndex( hypre_BoxIMax(cbox), ndim );

#if DEBUG_SYSBAMG_PFMG
    dxyz[cdir] *= 2;
#endif

    /* build the interpolation grid */
    hypre_SysBAMGCoarsen(PGrid_l[l], findex, stride, 0, &P_PGrid_l[l+1]);

    /* build the coarse grid */
    hypre_SysBAMGCoarsen(PGrid_l[l], cindex, stride, 1, &PGrid_l[l+1]);
  }

  num_levels = l + 1;

  hypre_printf("num_levels = %d\n", num_levels);

  /* set all levels active if skip_relax = 0 */
  if (!skip_relax) {
    for (l = 0; l < num_levels; l++) {
      active_l[l] = 1;
    }
  }

  (data->num_levels) = num_levels;

  sysbamg_ifdbg{hypre_printf("%s freeing\n", __func__); fflush(stdout);}

#if DEBUG_SYSBAMG_PFMG
  for ( i = 0; i < nvars; i++) {
    hypre_TFree(sys_dxyz[i]);
  }
  hypre_TFree(sys_dxyz);
#endif

  hypre_BoxDestroy(cbox);

  sysbamg_ifdbg{hypre_printf("%s finished\n", __func__); fflush(stdout);}

  return hypre_error_flag;
}

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGSetupMV
(
  hypre_SysBAMGData*      data,
  hypre_SStructPMatrix*   A,
  hypre_SStructPVector*   b,
  hypre_SStructPVector*   x
)
{
  MPI_Comm                comm              = (data->comm);
  HYPRE_Int               num_levels        = (data->num_levels);
  hypre_SStructPGrid**    PGrid_l           = (data->PGrid_l);
  hypre_SStructPGrid**    P_PGrid_l         = (data->P_PGrid_l);
  HYPRE_Int*              cdir_l            = (data->cdir_l);

  hypre_SStructPMatrix**  A_l               = (data->A_l);
  hypre_SStructPMatrix**  P_l               = (data->P_l);
  hypre_SStructPMatrix**  RT_l              = (data->RT_l);
  hypre_SStructPVector**  b_l               = (data->b_l);
  hypre_SStructPVector**  x_l               = (data->x_l);
  hypre_SStructPVector**  tx_l              = (data->tx_l);

  HYPRE_Int               l;

  /*----------------------------------------------------------------------------------------------
   * Create/Assemble matrix and vector structures
   *----------------------------------------------------------------------------------------------*/

  hypre_SStructPMatrixRef(A, &A_l[0]);
  hypre_SStructPVectorRef(b, &b_l[0]);
  hypre_SStructPVectorRef(x, &x_l[0]);

  hypre_SStructPVectorCreate(comm, PGrid_l[0], &tx_l[0]);
  hypre_SStructPVectorInitialize(tx_l[0]);

  for (l = 0; l < (num_levels - 1); l++)
  {
    P_l[l]  = hypre_SysBAMGCreateInterpOp(A_l[l], P_PGrid_l[l+1], cdir_l[l]);
    hypre_SStructPMatrixInitialize(P_l[l]);

    RT_l[l] = P_l[l];

    A_l[l+1] = hypre_SysBAMGCreateRAPOp(RT_l[l], A_l[l], P_l[l], PGrid_l[l+1], cdir_l[l]);
    hypre_SStructPMatrixInitialize(A_l[l+1]);

    hypre_SStructPVectorCreate(comm, PGrid_l[l+1], &b_l[l+1]);
    hypre_SStructPVectorInitialize(b_l[l+1]);

    hypre_SStructPVectorCreate(comm, PGrid_l[l+1], &x_l[l+1]);
    hypre_SStructPVectorInitialize(x_l[l+1]);

    hypre_SStructPVectorCreate(comm, PGrid_l[l+1], &tx_l[l+1]);
    hypre_SStructPVectorInitialize(tx_l[l+1]);
  }

  hypre_SStructPVectorAssemble(tx_l[0]);

  for (l = 1; l < num_levels; l++)
  {
    hypre_SStructPVectorAssemble(b_l[l]);
    hypre_SStructPVectorAssemble(x_l[l]);
    hypre_SStructPVectorAssemble(tx_l[l]);
  }

  sysbamg_ifdbg{hypre_printf("%s finished\n", __func__); fflush(stdout);}

  return hypre_error_flag;
}

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGSetupTV
(
  hypre_SysBAMGData*      data,
  hypre_SStructPVector*** tv,
  HYPRE_Real*             relax_weights
)
{
  MPI_Comm                comm              = (data->comm);
  HYPRE_Int               num_rtv           = (data->num_rtv);
  HYPRE_Int               num_stv           = (data->num_stv);
  HYPRE_Int               num_levels        = (data->num_levels);
  HYPRE_Int               symmetric         = (data->symmetric);

  hypre_SStructPGrid**    PGrid_l           = (data->PGrid_l);

  HYPRE_Int               num_tv            = num_rtv + num_stv;

  // these are = num_tv et al if A is symmetric and 2*num_tv et al if not
  HYPRE_Int               nsym              = ( data->symmetric ? 1 : 2 );

  HYPRE_Int               l, k;

  sysbamg_ifdbg
  {
     hypre_printf("%s:%d symmetric=%d num_tv*nsym=%d\n",
                  __FILE__, __LINE__, symmetric, num_tv*nsym);
     fflush(stdout);
  }

  for ( l = 0; l < num_levels; l++ )
  {
    tv[l] = hypre_TAlloc(hypre_SStructPVector*, num_tv*nsym);

    for ( k = 0; k < num_tv*nsym; k++ ) {
      hypre_SStructPVectorCreate(comm, PGrid_l[l], &tv[l][k]);
      hypre_SStructPVectorInitialize(tv[l][k]);
      hypre_SStructPVectorAssemble(tv[l][k]);
    }
  }

  for ( k = 0; k < num_rtv*nsym; k++ ) {
    // note: need offset (5173, arbitrary) so that tv[0][0] != tv[0][1] (on oslic at least)
    hypre_SStructPVectorSetRandomValues(tv[0][k], k + 5173 /*+ (HYPRE_Int)time(0)*/);

#if DEBUG_SYSBAMG > 1
    char filename[255];
    hypre_sprintf(filename, "sysbamg_tv_init,k=%d.dat", k);
    hypre_SStructPVectorPrint(filename, tv[0][k], 0);
#endif
  }

  sysbamg_ifdbg{hypre_printf("%s finished\n", __func__); fflush(stdout);}

  return hypre_error_flag;
}

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGSetupOperators
(
  hypre_SysBAMGData*      data,
  hypre_SStructPVector*** tv,
  HYPRE_Int               num_tv_,
  HYPRE_Real*             relax_weights,
  HYPRE_Int               cmaxsize
)
{
  HYPRE_Int*              cdir_l            = (data->cdir_l);
  hypre_SStructPMatrix**  A_l               = (data->A_l);
  hypre_SStructPMatrix**  P_l               = (data->P_l);
  hypre_SStructPMatrix**  RT_l              = (data->RT_l);
  hypre_SStructPVector**  b_l               = (data->b_l);
  hypre_SStructPVector**  x_l               = (data->x_l);
  hypre_SStructPVector**  e_l               = (data->e_l);
  hypre_SStructPVector**  r_l               = (data->r_l);
  hypre_SStructPVector**  tx_l              = (data->tx_l);
  hypre_SStructPGrid**    PGrid_l           = (data->PGrid_l);

  void**                  relax_data_l      = (data->relax_data_l);
  void**                  matvec_data_l     = (data->matvec_data_l);
  void**                  restrict_data_l   = (data->restrict_data_l);
  void**                  interp_data_l     = (data->interp_data_l);

  HYPRE_Int               num_levels        = (data->num_levels);
  HYPRE_Int               relax_type        = (data->relax_type);
  HYPRE_Int               usr_jacobi_weight = (data->usr_jacobi_weight);
  HYPRE_Real              jacobi_weight     = (data->jacobi_weight);
  HYPRE_Int               num_pre_relax_tv  = (data->num_pre_relax_tv);
  MPI_Comm                comm              = (data->comm);

  hypre_Index             cindex;
  hypre_Index             findex;
  hypre_Index             stride;

  HYPRE_Int               l, k;

  for (l = 0; l < (num_levels - 1); l++)
  {
    hypre_BAMGSetCIndex(cdir_l[l], cindex);
    hypre_BAMGSetFIndex(cdir_l[l], findex);
    hypre_BAMGSetStride(cdir_l[l], stride);

    // smooth the test vectors at *this* level
    {
      sysbamg_ifdbg{hypre_printf("smooth the test vectors at level %d\n", l); fflush(stdout);}

      // 1) set up the rhs for smoothing, zero for now
      hypre_SStructPVector* rhs;
      hypre_SStructPVectorCreate(comm, PGrid_l[l], &rhs);
      hypre_SStructPVectorInitialize(rhs);
      hypre_SStructPVectorAssemble(rhs);

      hypre_SStructPVectorSetConstantValues(rhs, 0.0);

      // 2) set up the relax struct
      void* tv_relax = hypre_SysBAMGRelaxCreate(comm);
      hypre_SysBAMGRelaxSetTol(tv_relax, 0.0);
      if (usr_jacobi_weight) {
        hypre_SysBAMGRelaxSetJacobiWeight(tv_relax, jacobi_weight);
      }
      else {
        hypre_SysBAMGRelaxSetJacobiWeight(tv_relax, relax_weights[l]);
      }
      hypre_SysBAMGRelaxSetType(tv_relax, relax_type);
      hypre_SysBAMGRelaxSetTempVec(tv_relax, tx_l[l]);
      hypre_SysBAMGRelaxSetPreRelax(tv_relax);
      hypre_SysBAMGRelaxSetMaxIter(tv_relax, num_pre_relax_tv);
      hypre_SysBAMGRelaxSetZeroGuess(tv_relax, 0);
      hypre_SysBAMGRelaxSetup(tv_relax, A_l[l], rhs, x_l[l]);

      // 3) smooth
      for ( k = 0; k < num_tv_; k++ ) {
        hypre_SysBAMGRelax( tv_relax, A_l[l], rhs, tv[l][k] );
      }

      // 4) destroy the relax struct
      hypre_SysBAMGRelaxDestroy( tv_relax );

      // 5) destroy the rhs
      hypre_SStructPVectorDestroy( rhs );

#if DEBUG_SYSBAMG > 1
      sysbamg_ifdbg
      {
         hypre_printf("printing sysbamg test vectors; level %d; num_tv_ %d\n", l, num_tv_);
         fflush(stdout);
      }
      char filename[255];
      for ( k = 0; k < num_tv_; k++ ) {
        hypre_sprintf(filename, "sysbamg_tv_l=%d,k=%d.dat", l, k);
        hypre_SStructPVectorPrint(filename, tv[l][k], 0);
      }
#endif
      fflush(stdout);
    }

    HYPRE_Int num_interps = num_levels-2;

    /* set up the interpolation operator */
    sysbamg_ifdbg
    {
       hypre_printf( "SysBAMGSetupInterpOp %d of %d\n", l, num_interps );
       fflush(stdout);
    }
    hypre_SysBAMGSetupInterpOp(A_l[l], cdir_l[l], findex, stride, P_l[l], num_tv_, tv[l]);

    /* set up the coarse grid operator */
    sysbamg_ifdbg
    {
       hypre_printf( "SysBAMGSetupRAPOp    %d of %d\n", l, num_interps );
       fflush(stdout);
    }
    hypre_SysBAMGSetupRAPOp(RT_l[l], A_l[l], P_l[l], cdir_l[l], cindex, stride, A_l[l+1]);

    /* set up the interpolation routine */
    sysbamg_ifdbg
    {
       hypre_printf( "SysSemiInterpSetup   %d of %d\n", l, num_interps );
       fflush(stdout);
    }
    hypre_SysSemiInterpSetup(interp_data_l[l], P_l[l], 0, x_l[l+1], e_l[l], cindex, findex, stride);

    /* set up the restriction routine */
    sysbamg_ifdbg
    {
       hypre_printf( "SysSemiRestrictSetup %d of %d\n", l, num_interps );
       fflush(stdout);
    }
    hypre_SysSemiRestrictSetup(restrict_data_l[l], RT_l[l], 1, r_l[l], b_l[l+1], cindex, findex, stride);

    // restrict the tv[l] to tv[l+1] (NB: don't need tv's on the coarsest grid)
    if ( l < num_interps ) {
      sysbamg_ifdbg
      {
         hypre_printf( "SysSemiRestrict      %d of %d\n", l, num_interps );
         fflush(stdout);
      }
      for ( k = 0; k < num_tv_; k++ ) {
        hypre_SysSemiRestrict(restrict_data_l[l], RT_l[l], tv[l][k], tv[l+1][k]);
      }
    }
  }

  // need to set A_l.'symmetric' for test-vector computations.
  // XXX hard-wiring
  for (l = 1; l < num_levels; l++)
    hypre_SStructPMatrixSetSymmetric( A_l[l], 0, 0, hypre_SStructPMatrixSymmetric(A_l[0])[0][0] );

  /* set up fine grid relaxation */
  hypre_SysBAMGRelaxSetTol(relax_data_l[0], 0.0);
  if (usr_jacobi_weight) {
    hypre_SysBAMGRelaxSetJacobiWeight(relax_data_l[0], jacobi_weight);
  }
  else {
    hypre_SysBAMGRelaxSetJacobiWeight(relax_data_l[0], relax_weights[0]);
  }
  hypre_SysBAMGRelaxSetType(relax_data_l[0], relax_type);
  hypre_SysBAMGRelaxSetTempVec(relax_data_l[0], tx_l[0]);
  hypre_SysBAMGRelaxSetup(relax_data_l[0], A_l[0], b_l[0], x_l[0]);
  if (num_levels > 1)
  {
    for (l = 1; l < num_levels; l++)
    {
      /* set relaxation parameters */
      hypre_SysBAMGRelaxSetTol(relax_data_l[l], 0.0);
      if (usr_jacobi_weight) {
        hypre_SysBAMGRelaxSetJacobiWeight(relax_data_l[l], jacobi_weight);
      }
      else {
        hypre_SysBAMGRelaxSetJacobiWeight(relax_data_l[l], relax_weights[l]);
      }
      hypre_SysBAMGRelaxSetType(relax_data_l[l], relax_type);
      hypre_SysBAMGRelaxSetTempVec(relax_data_l[l], tx_l[l]);
    }

    /* change coarsest grid relaxation parameters */
    /* do no more work on the coarsest grid than the cost of a V-cycle
     * (estimating roughly 4 communications per V-cycle level)
     * do sweeps proportional to the coarsest grid size */
    HYPRE_Int maxiter = hypre_min(4*num_levels, cmaxsize);
    hypre_SysBAMGRelaxSetType(relax_data_l[num_levels-1], 0);
    hypre_SysBAMGRelaxSetMaxIter(relax_data_l[num_levels-1], maxiter);

    /* call relax setup */
    for (l = 1; l < num_levels; l++) {
      hypre_SysBAMGRelaxSetup(relax_data_l[l], A_l[l], b_l[l], x_l[l]);
    }
  }

  /* set up the residual routine */
  for (l = 0; l < num_levels; l++) {
    hypre_SStructPMatvecSetup(matvec_data_l[l], A_l[l], x_l[l]);
  }

  sysbamg_ifdbg{hypre_printf("%s finished\n", __func__); fflush(stdout);}

  return hypre_error_flag;
}

/*--------------------------------------------------------------------------
 *--------------------------------------------------------------------------*/

HYPRE_Int hypre_SysBAMGCoarsen
(
 hypre_SStructPGrid*  finePGrid,
 hypre_Index          index,
 hypre_Index          stride,
 HYPRE_Int            prune,
 hypre_SStructPGrid** coarsePGrid_ptr
)
{
  hypre_SStructPGrid*   coarsePGrid;

  hypre_StructGrid*     fineSGrid;
  hypre_StructGrid*     coarseSGrid;

  MPI_Comm               comm;
  HYPRE_Int              ndim;
  HYPRE_Int              nvars;
  hypre_SStructVariable* vartypes;
  hypre_SStructVariable* new_vartypes;
  HYPRE_Int              i;
  HYPRE_Int              t;

  /*-----------------------------------------
   * Copy information from fine grid
   *-----------------------------------------*/

  comm      = hypre_SStructPGridComm(finePGrid);
  ndim      = hypre_SStructPGridNDim(finePGrid);
  nvars     = hypre_SStructPGridNVars(finePGrid);
  vartypes  = hypre_SStructPGridVarTypes(finePGrid);

  coarsePGrid = hypre_TAlloc(hypre_SStructPGrid, 1);

  hypre_SStructPGridComm(coarsePGrid)     = comm;
  hypre_SStructPGridNDim(coarsePGrid)     = ndim;
  hypre_SStructPGridNVars(coarsePGrid)    = nvars;
  new_vartypes = hypre_TAlloc(hypre_SStructVariable, nvars);
  for (i = 0; i < nvars; i++)
  {
    new_vartypes[i] = vartypes[i];
  }
  hypre_SStructPGridVarTypes(coarsePGrid) = new_vartypes;

  for (t = 0; t < 8; t++)
  {
    hypre_SStructPGridVTSGrid(coarsePGrid, t)     = NULL;
    hypre_SStructPGridVTIBoxArray(coarsePGrid, t) = NULL;
  }

  /*-----------------------------------------
   * Set the coarse SGrid
   *-----------------------------------------*/

  fineSGrid = hypre_SStructPGridCellSGrid(finePGrid);
  hypre_StructCoarsen(fineSGrid, index, stride, prune, &coarseSGrid);

  hypre_CopyIndex(hypre_StructGridPeriodic(coarseSGrid),
      hypre_SStructPGridPeriodic(coarsePGrid));

  hypre_SStructPGridSetCellSGrid(coarsePGrid, coarseSGrid);

  hypre_SStructPGridPNeighbors(coarsePGrid) = hypre_BoxArrayCreate(0, ndim);
  hypre_SStructPGridPNborOffsets(coarsePGrid) = NULL;

  hypre_SStructPGridLocalSize(coarsePGrid)  = 0;
  hypre_SStructPGridGlobalSize(coarsePGrid) = 0;
  hypre_SStructPGridGhlocalSize(coarsePGrid)= 0;

  hypre_SStructPGridAssemble(coarsePGrid);

  *coarsePGrid_ptr = coarsePGrid;

  return hypre_error_flag;
}
