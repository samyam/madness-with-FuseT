//#define WORLD_INSTANTIATE_STATIC_TEMPLATES
#include <madness.h>
#include <chem/nemo.h>
#include <chem/projector.h>

using namespace madness;

namespace madness {

class GaussianGuess : FunctionFunctorInterface<double,3> {
public:
    GaussianGuess(const Atom& atom, const double e, const int i, const int j,
            const int k) : x(atom.x), y(atom.y), z(atom.z),
            exponent(e), i(i), j(j), k(k) {
    }
    double x,y,z;
    double exponent;
    int i,j,k;  // cartesian exponents
    double operator()(const coord_3d& xyz) const {
        double xx=x-xyz[0];
        double yy=y-xyz[1];
        double zz=z-xyz[2];
        const double e=exponent*(xx*xx + yy*yy + zz*zz);
        return pow(xx,i)*pow(yy,j)*pow(zz,k)*exp(-e);
    }
};

std::vector<GaussianGuess> make_guess(const Molecule& mol, int maxl, const double exponent) {
    std::vector<GaussianGuess> gg;
    for (int i=0; i<mol.natom(); ++i) {
        print("atom ",i);
        const Atom& atom=mol.get_atom(i);
            print("l-quantum ",maxl);
            const int maxlp1=maxl+1;

            // loop over all cartesian components of l
            for (int i=0; i<1000; ++i) {
                std::vector<int> ijk(3);
                ijk[0]=i%maxlp1;
                ijk[1]=(i/maxlp1)%maxlp1;
                ijk[2]=(i/maxlp1/maxlp1)%maxlp1;
//                print("ijk");
//                print(ijk[0],ijk[1],ijk[2]);
                int current_l=ijk[0]+ijk[1]+ijk[2];
                if (current_l==maxl) {
                    gg.push_back(GaussianGuess(atom,exponent,ijk[0],ijk[1],ijk[2]));
                }
                if (ijk[0]+ijk[1]+ijk[2]==3*maxl) break;
            }
    }
    print("size of GaussianGuess",gg.size());
    return gg;
}

class QProjector {
public:
    QProjector(const vecfuncT& amo) : O(amo) {};
    real_function_3d operator()(const real_function_3d& rhs) const {
        return (rhs-O(rhs));
    }
    vecfuncT operator()(const vecfuncT& rhs) const {
        vecfuncT result(rhs.size());
        for (std::size_t i=0; i<rhs.size(); ++i) {
            result[i]=(rhs[i]-O(rhs[i])).truncate();
        }
        return result;
    }

private:
    Projector<double,3> O;
};

class Nuclear {
public:
    Nuclear(World& world, std::shared_ptr<NuclearCorrelationFactor> ncf)
        : world(world), ncf(ncf) {}

    real_function_3d operator()(const real_function_3d& ket) const {
        return ncf->apply_U(ket);
    }
    vecfuncT operator()(const vecfuncT& vket) const {
        vecfuncT result(vket.size());
        for (std::size_t i=0; i<vket.size(); ++i) result[i]=ncf->apply_U(vket[i]);
        return result;
    }

    double operator()(const real_function_3d& bra, const real_function_3d& ket) const {
        return inner(bra,this->operator()(ket));
    }

    Tensor<double> operator()(const vecfuncT& vbra, const vecfuncT& vket) const {
        vecfuncT vVket;
        for (std::size_t i=0; i<vket.size(); ++i) {
            vVket.push_back(this->operator()(vket[i]));
        }
        return matrix_inner(world,vbra,vVket);
    }

private:
    World& world;
    std::shared_ptr<NuclearCorrelationFactor> ncf;
};

class Kinetic {
public:
    Kinetic(World& world, const SCF& scf) : world(world), scf(scf) {}

    real_function_3d operator()(const real_function_3d ket) const {
        MADNESS_EXCEPTION("do not apply the kinetic energy operator on a function!",1);
        return ket;
    }

    double operator()(const real_function_3d& bra, const real_function_3d ket) const {
        double ke = 0.0;
        for (int axis = 0; axis < 3; axis++) {
            real_derivative_3d D = free_space_derivative<double, 3>(world,axis);
            const real_function_3d dket = D(ket);
            const real_function_3d dbra = D(bra);
            ke += 0.5 * (inner(dket, dbra));
        }
        return ke;
    }

    Tensor<double> operator()(const vecfuncT& vbra, const vecfuncT& vket) const {
        tensorT kinetic(vbra.size(),vket.size());
        distmatT dkinetic = scf.kinetic_energy_matrix(world,vbra,vket);
        dkinetic.copy_to_replicated(kinetic);
        return kinetic;
    }

private:
    World& world;
    const SCF& scf;
};

class Coulomb {
public:
    Coulomb(World& world, const vecfuncT& amo, SCF& calc, const real_function_3d& R2 )
        : world(world) {
        real_function_3d density=calc.make_density(world,calc.aocc,calc.amo);
        density.scale(2.0); // alpha + beta spin density
        density=density*R2;
        vcoul=calc.make_coulomb_potential(density);
    }
    real_function_3d operator()(const real_function_3d& ket) const {
        return vcoul*ket;
    }
    vecfuncT operator()(const vecfuncT& vket) const {
        return mul(world,vcoul,vket);
    }

    double operator()(const real_function_3d& bra, const real_function_3d ket) const {
        return inner(bra,vcoul*ket);
    }

    Tensor<double> operator()(const vecfuncT& vbra, const vecfuncT& vket) const {
        vecfuncT vJket;
        for (std::size_t i=0; i<vket.size(); ++i) {
            vJket.push_back(this->operator()(vket[i]));
        }
        return matrix_inner(world,vbra,vJket);
    }

private:
    real_function_3d vcoul; ///< the coulomb potential
    World& world;
};

class Exchange {
public:
    Exchange(World& world, const vecfuncT& amo, SCF& calc, const real_function_3d& R2 )
        : world(world), amo(amo), R2(R2) {
        poisson = std::shared_ptr<real_convolution_3d>(
                CoulombOperatorPtr(world, calc.param.lo, calc.param.econv));
    }
    real_function_3d operator()(const real_function_3d& ket) const {
        real_function_3d result = real_factory_3d(world).compressed(true);
        real_function_3d R2ket=R2*ket;
        for (std::size_t k = 0; k < amo.size(); ++k) {
            real_function_3d ik = amo[k] * R2ket;
            result += amo[k] * (*poisson)(ik);
        }
        return result;
    }

    vecfuncT operator()(const vecfuncT& vket) const {
        vecfuncT result(vket.size());
        for (std::size_t i=0; i<vket.size(); ++i) result[i]=this->operator()(vket[i]);
        return result;
    }

    double operator()(const real_function_3d& bra, const real_function_3d ket) const {
        return inner(bra,this->operator()(ket));
    }

    Tensor<double> operator()(const vecfuncT& vbra, const vecfuncT& vket) const {
        vecfuncT vKket;
        for (std::size_t i=0; i<vket.size(); ++i) {
            vKket.push_back(this->operator()(vket[i]));
        }
        return matrix_inner(world,vbra,vKket);
    }

private:
    World& world;
    const vecfuncT amo;
    const real_function_3d R2;
    std::shared_ptr<real_convolution_3d> poisson;
};


class Fock {
public:
    Fock(World& world, const Nemo& nemo) : world(world), nemo(nemo),
          J(world,nemo.get_calc()->amo,*(nemo.get_calc()),nemo.nuclear_correlation->square()),
          K(world,nemo.get_calc()->amo,*(nemo.get_calc()),nemo.nuclear_correlation->square()),
          T(world,*nemo.get_calc()),
          V(world,nemo.nuclear_correlation) {
    }
    real_function_3d operator()(const real_function_3d& ket) const {
        real_function_3d result;
        return result;
    }
    double operator()(const real_function_3d& bra, const real_function_3d ket) const {
        double J_00 = J(bra,ket);
        double K_00 = K(bra,ket);
        double T_00 = T(bra,ket);
        double V_00 = V(bra,ket);
        return T_00 + J_00 - K_00 + V_00;
    }

    Tensor<double> operator()(const vecfuncT& vbra, const vecfuncT& vket) const {
        Tensor<double> kmat=K(vbra,vket);
        Tensor<double> jmat=J(vbra,vket);
        Tensor<double> tmat=T(vbra,vket);
        Tensor<double> vmat=V(vbra,vket);
        Tensor<double> fock=tmat+jmat-kmat+vmat;
        return fock;
    }


private:
    World& world;
    const Nemo& nemo;
    Coulomb J;
    Exchange K;
    Kinetic T;
    Nuclear V;
};


class PNO {
public:
    typedef std::shared_ptr<operatorT> poperatorT;

    /// POD for PNO keywords
    struct Parameters {
        int npno;   ///< number of PNOs

        bool do_diagonal_fock;  ///< diagonalize the Fock matrix in the space of the virtuals
        bool do_multiplier; ///< use the lagrangian multiplier for orthogonality
        /// number of frozen orbitals; note the difference to the "pair" keyword where you
        /// request a specific orbital. Here you freeze lowest orbitals, i.e. if you find
        ///  freeze 1
        /// in the input file the 0th orbital is kept frozen, and orbital 1 is the first to
        /// be correlated.
        int freeze;

        int maxiter;    ///< max number of iterations

        /// ctor reading out the input file
        Parameters(const std::string& input) : npno(10),
                do_diagonal_fock(true), do_multiplier(false),
                freeze(0), maxiter(20) {

            // get the parameters from the input file
            std::ifstream f(input.c_str());
            position_stream(f, "pno");
            std::string s;

            while (f >> s) {
                if (s == "end") break;
                else if (s == "maxiter") f >> maxiter;
                else if (s == "freeze") f >> freeze;
                else if (s == "diagonal_fock") do_diagonal_fock=true;
                else if (s == "no_diagonal_fock") do_diagonal_fock=false;
                else if (s == "do_multiplier") do_multiplier=true;
                else if (s == "no_multiplier") do_multiplier=false;
                else if (s == "npno") f >> npno;
                else continue;
            }
        }

    };

    PNO(World& world, const Nemo& nemo, const std::string input) : world(world),
        param(input), nemo(nemo),
        J(world,nemo.get_calc()->amo,*(nemo.get_calc()),nemo.nuclear_correlation->square()),
        K(world,nemo.get_calc()->amo,*(nemo.get_calc()),nemo.nuclear_correlation->square()),
        T(world,*nemo.get_calc()),
        V(world,nemo.nuclear_correlation),
        F(world,nemo),
        Q(nemo.get_calc()->amo) {


        poisson = std::shared_ptr<real_convolution_3d>(
              CoulombOperatorPtr(world, nemo.get_calc()->param.lo,
                        nemo.get_calc()->param.econv));
        bsh = std::shared_ptr<real_convolution_3d>(
                BSHOperatorPtr3D(world, 1.e-8, nemo.get_calc()->param.lo,
                        nemo.get_calc()->param.econv));
        print("doing Lagrangian multiplier ",param.do_multiplier);
        print("doing diagonal Fock matrix  ",param.do_diagonal_fock);

    }

    void solve() const {
        const vecfuncT& amo=nemo.get_calc()->amo;

        // plot mos
        for (std::size_t i=0; i<amo.size(); ++i) {
            std::string name="amo"+stringify(i);
            plot_plane(world,amo[i],name);
        }

        double energy=0.0;
        for (int i=0; i<amo.size(); ++i) {
            for (int j=i; j<amo.size(); ++j) {
                energy+=solve_pair(i,j);
            }
        }

    }

    double solve_pair(const int i, const int j) const {
        const vecfuncT& amo=nemo.get_calc()->amo;

        vecfuncT virtuals1=guess_virtuals();
        vecfuncT virtuals;

        const int npno=std::min(param.npno,int(virtuals1.size()));
        for (int i=0; i<npno; ++i) virtuals.push_back(virtuals1[i]);
        print("number of virtuals",virtuals.size());
        orthonormalize_cholesky(virtuals);

        // compute the energies of the occupied orbitals i and j
        const double f_ii=F(amo[i],amo[j]);
        const double f_jj=F(amo[j],amo[j]);
        print("diagonal fock matrix elements ",f_ii,f_jj);
        Tensor<double> amplitudes;

        for (int iter=0; iter<param.maxiter; iter++) {

            // recompute intermediates
            vecfuncT V_aj_i=compute_V_aj_i(amo[i],amo[j],virtuals);
            Tensor<double> fmat=F(virtuals,virtuals);

            // compute energy (might resort virtuals and intermediates)
            double e=compute_hylleraas(V_aj_i,virtuals,f_ii+f_jj,fmat,amplitudes);

            if (world.rank() == 0) printf("in iteration %2d at time %6.1f: %12.8f\n",iter, wall_time(),e);
            for (std::size_t i=0; i<virtuals.size(); ++i) {
                std::string name="virtual"+stringify(i)+"_iteration"+stringify(iter);
                plot_plane(world,virtuals[i],name);
            }
            // will change virtuals and invalidate V_aj_i
            virtuals=update_virtuals(V_aj_i,virtuals,f_ii+f_jj,fmat,amplitudes);
        }

        // compute the fock matrix of the virtuals
        Tensor<double> fmat=F(virtuals,virtuals);
        vecfuncT V_aj_i=compute_V_aj_i(amo[i],amo[j],virtuals);
        double e=compute_hylleraas(V_aj_i,virtuals,f_ii+f_jj,fmat,amplitudes);
        return e;
    }

    /// update the virtual functions

    /// @param[in]  amplitudes  the amplitudes t_a
    vecfuncT update_virtuals(vecfuncT& V_aj_i, const vecfuncT& virtuals,
            const double e0, const Tensor<double>& fmat,
            const Tensor<double> amplitudes) const {

        const int nvir=virtuals.size();
        for (std::size_t a=0; a<V_aj_i.size(); ++a) V_aj_i[a].scale(1./amplitudes(a));

        // compute the coupling term \sum_b |b> ...
        Tensor<double> transf(nvir,nvir);
        for (int a=0; a<fmat.dim(0); ++a) {
            for (int b=0; b<fmat.dim(1); ++b) {
                transf(a,b)=-fmat(a,b)*amplitudes(a)/amplitudes(b);
            }
            transf(a,a)=0.0;
        }

        // combine coupling and inhomogeneous term
        vecfuncT btilde=transform(world,virtuals,transf);
        btilde=sub(world,btilde,V_aj_i);

        // compute (J-K+V) | a >
        vecfuncT Ja=J(virtuals);
        vecfuncT Ka=K(virtuals);
        vecfuncT Va=V(virtuals);
        vecfuncT JKVa=add(world,sub(world,Ja,Ka),Va);
        truncate(world,JKVa);

        vecfuncT Vpsi=sub(world,btilde,JKVa);
        scale(world,Vpsi,2.0);
        vecfuncT GVpsi;

        Tensor<double> evals(fmat.dim(0));
        for (long a=0; a<evals.size(); ++a) evals(a)=e0-fmat(a,a);
        std::vector<poperatorT> bsh3= nemo.get_calc()->make_bsh_operators(world,evals);
        GVpsi=apply(world,bsh3,Vpsi);

        // post-processing: project out occ space, orthogonalize
        GVpsi=Q(GVpsi);
        orthonormalize_cholesky(GVpsi);
        check_orthonormality(GVpsi);

        return GVpsi;
    }

    /// guess a set up virtual orbitals -- currently from the minimal AO basis
    vecfuncT guess_virtuals() const {
        vecfuncT virtuals=guess_virtuals_from_gaussians();
        virtuals=Q(virtuals);
//        orthonormalize_cholesky(virtuals);
        return virtuals;
    }

    /// return a shell of l-quantum l and exponent e
    vecfuncT guess_virtual_gaussian_shell(const int l, const double e) const {
        vecfuncT virtuals;
        std::vector<GaussianGuess> gg=make_guess(nemo.molecule(),l,e);
        for (int m=0; m<gg.size(); ++m) {
            virtuals.push_back(real_factory_3d(world).functor2(gg[m]).truncate_on_project());
        }
        normalize(world,virtuals);
        return virtuals;
    }

    /// add guess virtuals as principal expansion
    vecfuncT guess_virtuals_from_gaussians() const {

        vecfuncT virtuals;
        append(virtuals,guess_virtual_gaussian_shell(0,1.0));

        append(virtuals,guess_virtual_gaussian_shell(0,2.0));
        append(virtuals,guess_virtual_gaussian_shell(1,1.0));

//        append(virtuals,guess_virtual_gaussian_shell(0,3.0)); // spherical vs cartesian
        append(virtuals,guess_virtual_gaussian_shell(1,2.0));
        append(virtuals,guess_virtual_gaussian_shell(2,1.0));

//        append(virtuals,guess_virtual_gaussian_shell(0,4.0)); // spherical vs cartesian
//        append(virtuals,guess_virtual_gaussian_shell(1,3.0)); // spherical vs cartesian
        append(virtuals,guess_virtual_gaussian_shell(2,2.0));
        append(virtuals,guess_virtual_gaussian_shell(3,1.0));

//        append(virtuals,guess_virtual_gaussian_shell(0,0.5)); // spherical vs cartesian
//        append(virtuals,guess_virtual_gaussian_shell(1,0.5)); // spherical vs cartesian
        append(virtuals,guess_virtual_gaussian_shell(2,0.5));
        append(virtuals,guess_virtual_gaussian_shell(3,0.5));
        print("number of guess virtuals: ",virtuals.size());
        return virtuals;
    }

    void append(vecfuncT& v, const vecfuncT& rhs) const {
        for (std::size_t i=0; i<rhs.size(); ++i) v.push_back(rhs[i]);
    }

    /// compute the function V_{\bar a j} | i> = - \int \dr' (-J12+K12+1/|r-r'|) a(r') j(r')

    /// the terms are expanded as follows:
    /// (-J1 +K1) | i(1) >  < a(2) | j(2) >
    ///  +  | i(1) > < a(2) | -J(2) + K(2) | j(2) >
    ///  +  i(1) * \int \dr2 1/|r12| a(2) j(2)
    /// the first line is zero due to orthogonality, the second line drops out
    /// due to the orthogonality projector.
    vecfuncT compute_V_aj_i(const real_function_3d& phi_i,
            const real_function_3d& phi_j, const vecfuncT& virtuals_bar) const {

        const vecfuncT aj=mul(world,phi_j,virtuals_bar);    // multiply \bar a j
        vecfuncT gaj=apply(world,*poisson,aj);        // \int \dr2 aj(2)/r12
        vecfuncT Vaj_i=mul(world,phi_i,gaj);
        vecfuncT Vaj_i1=Q(Vaj_i);
        truncate(world,Vaj_i1);
        return Vaj_i1;
    }


    /// compute the Hylleraas functional and the corresponding energy

    /// the virtuals are sorted according to their energy, so that some
    /// intermediates need to be resorted as well, in particular the V_aj_i
    /// intermediate and the Fock matrix. Its contents remain unchanged.
    /// @param[in]  amo occupied orbitals
    /// @param[inout]  virtuals    the optimal virtual orbitals; sorted upon exit
    /// @param[in]  e0  the zeroth-order energy (e_i + e_j)
    /// @param[out] t the optimized amplitudes
    /// @return the total energy of this pair
    double compute_hylleraas(vecfuncT& V_aj_i, vecfuncT& virtuals,
            const double e0, Tensor<double>& fmat, Tensor<double>& t) const {

        Tensor<double> V=inner(world,virtuals,V_aj_i);

        // compute the B matrix <a \bar a | F1 + F2 - E | b \bar b>
        const std::size_t nvir=virtuals.size();
        Tensor<double> B(nvir,nvir);
        for (std::size_t i=0; i<nvir; ++i) B(i,i)=fmat(i,i) *2.0 - e0;

        Tensor<double> Binv=inverse(B);

        t=-inner(Binv,V);
        V.emul(t);
        if (sort_virtuals(virtuals,V_aj_i,V,t)) fmat=F(virtuals,virtuals);
        return V.sum();
    }

    struct sort_helper {
        sort_helper(const real_function_3d& v1, const real_function_3d& Vaji,
                double e, double a)
            : v(v1), V_aj_i(Vaji), energy(e), amplitude(a) {}
        real_function_3d v;
        real_function_3d V_aj_i;
        double energy;
        double amplitude;
    };

    static bool comp(const sort_helper& rhs, const sort_helper& lhs) {
        return rhs.energy<lhs.energy;
    }

    /// sort the virtuals according to their pair energies

    /// @return if the virtuals have been resorted
    bool sort_virtuals(vecfuncT& v, vecfuncT& V_aj_i,
            Tensor<double>& VT, Tensor<double>& t) const {
        std::vector<sort_helper> pairs;
        for (std::size_t i=0; i<v.size(); ++i) {
            pairs.push_back(sort_helper(copy(v[i]),V_aj_i[i],VT(i),t(i)));
        }
        if (std::is_sorted(pairs.begin(),pairs.end(),PNO::comp)) {
            return false;
        } else {
            std::sort(pairs.begin(),pairs.end(),PNO::comp);
            for (std::size_t i=0; i<v.size(); ++i) {
                v[i]=pairs[i].v;
                t(i)=pairs[i].amplitude;
                VT(i)=pairs[i].energy;
                V_aj_i[i]=pairs[i].V_aj_i;
            }
        }
        return true;
    }


    void orthonormalize(vecfuncT& v) const {
        Tensor<double> ovlp=matrix_inner(world,v,v);
        Tensor<double> U, evals;
        syev(ovlp,U,evals);
        v=transform(world,v,U);
        normalize(world,v);
    }

    void orthonormalize_gram_schmidt(vecfuncT& v) const {
        print("Gram-Schmidt orthonormalization");
        for (std::size_t i=0; i<v.size(); ++i) {
            for (std::size_t j=0; j<i; ++j) {
                double ovlp=inner(v[i],v[j]);
                v[i]-=ovlp*v[j];
            }
            normalize(world,v);
        }
        normalize(world,v);
    }

    void check_orthonormality(const vecfuncT& v) const {
        Tensor<double> ovlp=matrix_inner(world,v,v);
        for (int i=0; i<ovlp.dim(0); ++i) ovlp(i,i)-=1.0;
        double error=ovlp.normf()/ovlp.size();
        if (error>1.e-14) print("orthonormality error: ",error);
    }

    void orthonormalize_cholesky(vecfuncT& v) const {
        Tensor<double> ovlp=matrix_inner(world,v,v);
        cholesky(ovlp); // destroys ovlp
        Tensor<double> L=transpose(ovlp);
        Tensor<double> Linv=inverse(L);
        Tensor<double> U=transpose(Linv);
        v=transform(world,v,U);
    }

    void orthonormalize_fock(const Tensor<double>& fmat, vecfuncT& v) const {
        Tensor<double> U, evals;
        syev(fmat,U,evals);
        v=transform(world,v,U);
        normalize(world,v);
    }

    void orthonormalize_fock2(Tensor<double>& fmat,
            const Tensor<double>& smat, vecfuncT& v) const {
        Tensor<double> U, evals;
        sygv(fmat,smat,1,U,evals);
        vecfuncT vnew=transform(world,v,U);
        normalize(world,vnew);
        fmat=0.0;
        for (int i=0; i<fmat.dim(0); ++i) fmat(i,i)=evals(i);
        // fix phases
        Tensor<double> ovlp=inner(world,vnew,v);
        for (std::size_t i=0; i<v.size(); ++i) {
            if (fabs(ovlp(i)-1.0)>1.e-4) print("faulty overlap",i,ovlp(i));
            if (ovlp(i)<0.0) vnew[i].scale(-1.0);
        }
        v=vnew;
    }


private:

    World& world;
    Parameters param;   ///< calculation parameters
    Nemo nemo;
    Coulomb J;
    Exchange K;
    Kinetic T;
    Nuclear V;
    Fock F;
    QProjector Q;
    std::shared_ptr<real_convolution_3d> poisson;
    std::shared_ptr<real_convolution_3d> bsh;

};
}


int main(int argc, char** argv) {
    initialize(argc, argv);
    World world(SafeMPI::COMM_WORLD);
    if (world.rank() == 0) printf("starting at time %.1f\n", wall_time());
    startup(world,argc,argv);
    std::cout.precision(6);

    const std::string input="input";
    std::shared_ptr<SCF> calc(new SCF(world,input.c_str()));
    Nemo nemo(world,calc);
    const double energy=nemo.value();
    if (world.rank()==0) print("nemo energy: ",energy);
    if (world.rank() == 0) printf(" at time %.1f\n", wall_time());
    
    const vecfuncT nemos=nemo.get_calc()->amo;
    const vecfuncT R2nemos=mul(world,nemo.nuclear_correlation->square(),nemos);

    Fock F(world,nemo);
    Tensor<double> fmat=F(nemos,nemos);
    print("Fock matrix");
    print(fmat);

    PNO pno(world,nemo,input);
    pno.solve();

    if (world.rank() == 0) printf("finished at time %.1f\n", wall_time());
    finalize();
    return 0;
}

