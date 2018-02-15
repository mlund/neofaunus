#pragma once

#include "core.h"
#include "geometry.h"
#include "space.h"
#include "potentials.h"
#include "multipole.h"
#include "penalty.h"
#include <set>

#ifdef FAU_POWERSASA
#include <power_sasa.h>
#endif

namespace Faunus {
    namespace Energy {

        class Energybase {
            public:
                enum keys {OLD, NEW, NONE};
                keys key=NONE;
                std::string name;
                std::string cite;
                virtual double energy(Change&)=0; //!< energy due to change
                inline virtual void to_json(json &j) const {}; //!< json output
                inline virtual void sync(Energybase*, Change&) {}
        };

        void to_json(json &j, const Energybase &base) {
            assert(!base.name.empty());
            if (!base.cite.empty())
                j[base.name]["reference"] = base.cite;
            base.to_json( j[base.name] );
        } //!< Converts any energy class to json object

        /**
         * This holds Ewald setup and must *not* depend on particle type, nor depend on Space
         */
        struct EwaldData {
            typedef std::complex<double> Tcomplex;
            typedef std::vector<Tcomplex> Tvecx;
            Tvecx Qion;
            Tvecx Qdip;
            Eigen::MatrixXd kVectors; // Matrices with k-vectors
            Eigen::VectorXd Aks;      // values based on k-vectors to minimize computational effort (Eq.24,DOI:10.1063/1.481216)
            double alpha, alpha2, rc, kc, kc2, check_k2_zero, lB;
            double const_inf, eps_surf;
            bool ionion;
            bool iondipole;
            bool dipoledipole;
            bool spherical_sum;
            bool ipbc;
            int kcc;
            int kVectorsInUse=0;
            Point L; //!< Box dimensions

            void update(const Point &box) {
                L = box;
                check_k2_zero = 0.1*std::pow(2*pc::pi/L.maxCoeff(), 2);
                int kVectorsLength = (2*kcc+1) * (2*kcc+1) * (2*kcc+1) - 1;
                if (kVectorsLength == 0) {
                    kVectors.resize(3, 1); 
                    Aks.resize(1);
                    kVectors.col(0) = Point(1,0,0); // Just so it is not the zero-vector
                    Aks[0] = 0;
                    kVectorsInUse = 1;
                    Qion.resize(1);
                    Qdip.resize(1);
                } else {
                    kVectors.resize(3, kVectorsLength); 
                    Aks.resize(kVectorsLength);
                    kVectorsInUse = 0;
                    kVectors.setZero();
                    Aks.setZero();
                    int startValue = 1 - int(ipbc);
                    double factor = 1;
                    for (int kx = 0; kx <= kcc; kx++) {
                        if(kx > 0)
                            factor = 2;
                        double dkx2 = double(kx*kx);
                        for (int ky = -kcc*startValue; ky <= kcc; ky++) {
                            double dky2 = double(ky*ky);
                            for (int kz = -kcc*startValue; kz <= kcc; kz++) {
                                double dkz2 = double(kz*kz);
                                Point kv = 2*pc::pi*Point(kx/L.x(),ky/L.y(),kz/L.z());
                                double k2 = kv.dot(kv);
                                if (k2 < check_k2_zero) // Check if k2 != 0
                                    continue;
                                if (spherical_sum)
                                    if( (dkx2/kc2) + (dky2/kc2) + (dkz2/kc2) > 1)
                                        continue;
                                kVectors.col(kVectorsInUse) = kv; 
                                Aks[kVectorsInUse] = factor*std::exp(-k2/(4*alpha2))/k2;
                                kVectorsInUse++;
                            }
                        }
                    }
                    Qion.resize(kVectorsInUse);
                    Qdip.resize(kVectorsInUse);
                }
            }
        };

        void from_json(const json &j, EwaldData &d) {
            d.alpha = j.at("alpha");
            d.alpha2 = d.alpha*d.alpha;
            d.rc = j.at("cutoff");
            d.kc = j.at("cutoffK");
            d.kc2 = d.kc*d.kc;
            d.kcc = std::ceil(d.kc);
            d.ipbc = j.value("ipbc", false);
            d.spherical_sum = j.value("spherical_sum", true);
            d.lB = pc::lB( j.at("epsr") );
	    d.eps_surf = j.value("epss", 0.0);
            d.const_inf = (d.eps_surf < 1) ? 0 : 1; // if unphysical (<1) use epsr infinity for surrounding medium
        }

        void to_json(json &j, const EwaldData &d) {
            j["lB"] = d.lB;
            j["ipbc"] = d.ipbc;
            j["epss"] = d.eps_surf;
            j["alpha"] = d.alpha;
            j["cutoff"] = d.rc;
            j["cutoffk"] = d.kc;
            j["wavefunctions"] = d.kVectorsInUse;
            j["spherical_sum"] = d.spherical_sum;
            // more: number of k-vectors etc.
        }

        /** @brief recipe or policies for ion-ion ewald */
        template<class Tspace>
            struct PolicyIonIon  {
                typedef typename Tspace::Tpvec::iterator iter;
                Tspace *spc;
                Tspace *old=nullptr; // set only if key==NEW at first call to `sync()`

                PolicyIonIon(Tspace &spc) : spc(&spc) {}

                void updateComplex(EwaldData &data) const {
                    for (int k=0; k<data.kVectorsInUse; k++) {
                        const Point& kv = data.kVectors.col(k);
                        EwaldData::Tcomplex Q(0,0);
                        for (auto &i : spc->p) {
                            double dot = kv.dot(i.pos);
                            Q += i.charge * EwaldData::Tcomplex( std::cos(dot), std::sin(dot) );
                        }
                        data.Qion.at(k) = Q;
                    }
                } //!< Update all k vectors

                void updateComplex(EwaldData &data, iter begin, iter end) const {
                    assert(old!=nullptr);
                    assert(spc->p.size() == old->p.size());
                    size_t ibeg = std::distance(spc->p.begin(), begin); // it->index
                    size_t iend = std::distance(spc->p.begin(), end);   // it->index
                    for (int k=0; k<data.kVectorsInUse; k++) {
                        auto& Q = data.Qion.at(k);
                        Point q = data.kVectors.col(k);
                        for (size_t i=ibeg; i<=iend; i++) {
                            double _new = q.dot(spc->p[i].pos);
                            double _old = q.dot(old->p[i].pos);
                            Q += spc->p[i].charge * EwaldData::Tcomplex( std::cos(_new), std::sin(_new) );
                            Q -= old->p[i].charge * EwaldData::Tcomplex( std::cos(_old), std::sin(_old) );
                        }
                    }
                } //!< Optimized update of k subset. Require access to old positions through `old` pointer

                double selfEnergy(const EwaldData &d) {
                    double E = 0;
                    for (auto& i : spc->p)
                        E += i.charge * i.charge;
                    return -d.alpha*E / std::sqrt(pc::pi) * d.lB;
                }

                double surfaceEnergy(const EwaldData &d) {
                    if (d.const_inf < 0.5)
                        return 0;
                    Point qr(0,0,0);
                    for (auto &i : spc->p)
                        qr += i.charge*i.pos;
                    return d.const_inf * 2 * pc::pi / ( (2*d.eps_surf+1) * spc->geo.getVolume() ) * qr.dot(qr) * d.lB;
                }

                double reciprocalEnergy(const EwaldData &d) {
                    double E = 0;
                    for (size_t k=0; k<d.Qion.size(); k++)
                        E += d.Aks[k] * std::norm( d.Qion[k] );
                    return 2 * pc::pi / spc->geo.getVolume() * E * d.lB;
                }
            };

        /** @brief Ewald summation reciprocal energy */
        template<class Tspace, class Policy=PolicyIonIon<Tspace>>
            class Ewald : public Energybase {
                private:
                    EwaldData data;
                    Policy policy;
                public:
                    Tspace& spc;

                    Ewald(const json &j, Tspace &spc) : spc(spc), policy(spc) {
                        name = "ewald";
                        data = j;
                        data.update( spc.geo.getLength() );
                        policy.updateComplex(data); // brute force. todo: be selective
                    }

                    double energy(Change &change) override {
                        double u=0;
                        if (!change.empty()) {
                            // If the state is NEW (trial state), then update all k-vectors
                            if (key==NEW) {
                                if (change.all || change.dV)       // everything changes
                                    policy.updateComplex(data);    // update all (expensive!)
                                else {
                                    if (change.groups.size()==1) { // exactly one group is moved
                                        auto& d = change.groups[0];
                                        auto& g = spc.groups[d.index];
                                        if (d.atoms.size()==1)     // exactly one atom is moved
                                            policy.updateComplex(data, g.begin()+d.atoms[0], g.begin()+d.atoms[0]);
                                        else
                                            policy.updateComplex(data, g.begin(), g.end());
                                    } else
                                        policy.updateComplex(data);
                                }
                            }
                            u = policy.selfEnergy(data) + policy.surfaceEnergy(data) + policy.reciprocalEnergy(data); 
                        }
                        return u;
                    }

                    void sync(Energybase *basePtr, Change &change) override {
                        auto other = dynamic_cast<decltype(this)>(basePtr);
                        assert(other);
                        if (other->key==OLD)
                            policy.old = &(other->spc); // give NEW access to OLD space for optimized updates
                        data = other->data; // copy everything!

                    } //!< Called after a move is rejected/accepted as well as before simulation

                    void to_json(json &j) const override {
                        j = data;
                    }
            };

        template<typename Tspace>
            class Isobaric : public Energybase {
                private:
                    Tspace& spc;
                    double P; // P/kT
                public:
                    Isobaric(const json &j, Tspace &spc) : spc(spc) {
                        name = "isobaric";
                        cite = "Frenkel & Smith 2nd Ed (Eq. 5.4.13)";
                        P = j.value("P/mM", 0.0) * 1.0_mM;
                        if (P<1e-10) {
                            P = j.value("P/Pa", 0.0) * 1.0_Pa;
                            if (P<1e-10)
                                P = j.at("P/atm").get<double>() * 1.0_atm;
                        }
                    }
                    double energy(Change &change) override {
                        if (change.dV || change.all) {
                            double V = spc.geo.getVolume();
                            size_t N=0;
                            for (auto &g : spc.groups)
                                if (!g.empty()) {
                                    if (g.atomic)
                                        N += g.size();
                                    else
                                        N++;
                                }
                            return P*V-(N+1)*std::log(V);
                        } else return 0; 
                    }
                    void to_json(json &j) const override {
                        j["P/atm"] = P / 1.0_atm;
                        j["P/mM"] = P / 1.0_mM;
                        j["P/Pa"] = P / 1.0_Pa;
                        _roundjson(j,5);
                    }
            };

        template<typename Tspace>
            class ExternalPotential : public Energybase {
                protected:
                    typedef typename Tspace::Tpvec Tpvec;
                    typedef typename Tspace::Tparticle Tparticle;
                    Tspace& spc;
                    std::set<int> molids; // molecules to act upon
                    std::function<double(const Tparticle&)> func=nullptr; // energy of single particle
                    std::vector<std::string> _names;

                    template<class Tparticle>
                        double _energy(const Group<Tparticle> &g) const {
                            double u=0;
                            if (molids.find(g.id)!=molids.end())
                                for (auto &p : g) {
                                    u += func(p);
                                    if (std::isnan(u))
                                        break;
                                }
                            return u;
                        } //!< External potential on a single particle
                public:
                    ExternalPotential(const json &j, Tspace &spc) : spc(spc) {
                        name="external";
                        _names = j.at("molecules").get<decltype(_names)>(); // molecule names
                        auto _ids = names2ids(molecules<Tpvec>, _names);     // names --> molids
                        molids = std::set<int>(_ids.begin(), _ids.end());    // vector --> set
                        if (molids.empty() || molids.size()!=_names.size() )
                            throw std::runtime_error(name + ": molecule list is empty");
                    }

                    double energy(Change &change) override {
                        assert(func!=nullptr);
                        double u=0;
                        if (change.dV || change.all) {
                            for (auto &g : spc.groups) { // check all groups
                                u += _energy(g);
                                if (std::isnan(u))
                                    break;
                            }
                        } else
                            for (auto &d : change.groups) {
                                auto &g = spc.groups.at(d.index); // check specified groups
                                if (d.all)  // check all atoms in group
                                    u += _energy(g);
                                else {       // check only specified atoms in group
                                    if (molids.find(g.id)!=molids.end())
                                        for (auto i : d.atoms)
                                            u += func( *(g.begin()+i) );
                                }
                                if (std::isnan(u))
                                    break;
                            }
                        return u;
                    }

                    void to_json(json &j) const override {
                        j["molecules"] = _names;
                    }
            }; //!< Base class for external potentials, acting on particles

        template<typename Tspace, typename base=ExternalPotential<Tspace>>
            class Confine : public base {
                public:
                    enum Variant {sphere, cylinder, cuboid, none};
                    Variant type=none;

                private:
                    Point origo={0,0,0}, dir={1,1,1};
                    Point low, high;
                    double radius, k;
                    bool scale=false;
                    std::map<std::string, Variant> m = {
                        {"sphere", sphere}, {"cylinder", cylinder}, {"cuboid", cuboid}
                    };

                public:
                    Confine(const json &j, Tspace &spc) : base(j,spc) {
                        base::name = "confine";
                        k = value_inf(j, "k") * 1.0_kJmol; // get floating point; allow inf/-inf
                        type = m.at( j.at("type") );

                        if (type==sphere || type==cylinder) {
                            radius = j.at("radius");
                            origo = j.value("origo", origo);
                            scale = j.value("scale", scale);
                            if (type==cylinder)
                                dir = {1,1,0};
                            base::func = [&radius=radius, origo=origo, k=k, dir=dir](const typename base::Tparticle &p) {
                                double d2 = (origo-p.pos).cwiseProduct(dir).squaredNorm() - radius*radius;
                                if (d2>0)
                                    return 0.5*k*d2;
                                return 0.0;
                            };

                            // If volume is scaled, also scale the confining radius by adding a trigger
                            // to `Space::scaleVolume()`
                            if (scale)
                                spc.scaleVolumeTriggers.push_back( [&radius=radius](Tspace &spc, double Vold, double Vnew) {
                                        radius *= std::cbrt(Vnew/Vold); } );
                        }

                        if (type==cuboid) {
                            low = j.at("low").get<Point>();
                            high = j.at("high").get<Point>();
                            base::func = [low=low, high=high, k=k](const typename base::Tparticle &p) {
                                double u=0;
                                Point d = low-p.pos;
                                for (int i=0; i<3; ++i)
                                    if (d[i]>0) u+=d[i]*d[i];
                                d = p.pos-high;
                                for (int i=0; i<3; ++i)
                                    if (d[i]>0) u+=d[i]*d[i];
                                return 0.5*k*u;
                            };
                        }
                    }

                    void to_json(json &j) const override {
                        if (type==cuboid)
                            j = {{"low", low}, {"high", high}};
                        if (type==sphere || type==cylinder)
                            j = {{"radius", radius}};
                        if (type==sphere) {
                            j["origo"] = origo;
                            j["scale"] = scale;
                        }
                        for (auto &i : m)
                            if (i.second==type)
                                j["type"] = i.first;
                        j["k"] = k/1.0_kJmol;
                        base::to_json(j);
                        _roundjson(j,5);
                    }
            }; //!< Confine particles to a sub-region of the simulation container

        template<typename Tspace>
            class Bonded : public Energybase {
                private:
                    typedef typename Tspace::Tpvec Tpvec;
                    Tspace& spc;
                    typedef std::vector<Potential::BondData> BondVector;
                    BondVector inter;  // inter-molecular bonds
                    std::map<int,BondVector> intra; // intra-molecular bonds

                    void update() {
                        for (size_t i=0; i<spc.groups.size(); i++) {
                            auto &g = spc.groups[i];
                            intra[i] = molecules<Tpvec>.at(g.id).bonds;
                            for (auto &b : intra[i])
                                b.shift( std::distance(spc.p.begin(), g.begin()) );
                        }
                    }

                    double sum( const BondVector &v ) const {
                        double u=0;
                        for (auto &b : v)
                            u += b.energy(spc.p, spc.geo.distanceFunc);
                        return u;
                    }

                public:
                    Bonded(const json &j, Tspace &spc) : spc(spc) {
                        name = "bonded";
                        update();
                        if (j.is_object())
                            if (j.count("bondlist")==1)
                                inter = j["bondlist"].get<BondVector>();
                    }
                    void to_json(json &j) const override {
                        if (!inter.empty())
                            j["bondlist"] = inter;
                        if (!intra.empty()) {
                            json& _j = j["bondlist-intramolecular"];
                            _j = json::array();
                            for (auto &i : intra)
                                for (auto &b : i.second)
                                    _j.push_back(b);
                        }
                    }

                    double energy(Change &c) override {
                        double u=0;
                        if ( !c.empty() ) {
                            u = sum(inter); // energy of inter-molecular bonds
                            if ( c.all || c.dV )
                                for (auto& i : intra) // energy of intra-molecular bonds
                                    u += sum(i.second);
                            else
                                for (auto &d : c.groups)
                                    u += sum( intra[d.index] );
                        }
                        return u;
                    }; // brute force -- refine this!
            };

        /**
         * @brief Nonbonded energy using a pair-potential
         */
        template<typename Tspace, typename Tpairpot>
            class Nonbonded : public Energybase {
                private:
                    double g2gcnt=0, g2gskip=0;
                protected:
                    typedef typename Tspace::Tgroup Tgroup;
                    double Rc2_g2g=pc::infty;

                    void to_json(json &j) const override {
                        j = pairpot;
                        json t = json::object();
                        t["g2g"] = { {"cutoff", std::sqrt(Rc2_g2g)} };
                        //t["cutoff_g2g"] = std::sqrt(Rc2_g2g);
                        j.push_back(t);
                    }

                    template<typename T>
                        inline bool cut(const T &g1, const T &g2) {
                            g2gcnt++;
                            if ( spc.geo.sqdist(g1.cm, g2.cm)<Rc2_g2g )
                                return false;
                            g2gskip++;
                            return true;
                        } //!< true if group<->group interaction can be skipped

                    template<typename T>
                        inline double i2i(const T &a, const T &b) {
                            return pairpot(a, b, spc.geo.vdist(a.pos, b.pos));
                        }

                    template<typename T>
                        double g_internal(const T &g) {
                            double u=0;
                            for ( auto i = g.begin(); i != g.end(); ++i )
                                for ( auto j=i; ++j != g.end(); )
                                    u += i2i(*i, *j);
                            return u;
                        }

                    virtual double g2g(const Tgroup &g1, const Tgroup &g2) {
                        double u = 0;
                        if (!cut(g1,g2))
                            for (auto &i : g1)
                                for (auto &j : g2)
                                    u += i2i(i,j);
                        return u;
                    }

                    template<typename T>
                        double g2all(const T &g1) {
                            double u = 0;
                            for ( auto i = spc.groups.begin(); i != spc.groups.end(); ++i ) {
                                for ( auto j=i; ++j != spc.groups.end(); )
                                    u += g2g( *i, *j );
                                return u;
                            }
                        }

                public:
                    Tspace& spc;   //!< Space to operate on
                    Tpairpot pairpot; //!< Pair potential

                    Nonbonded(const json &j, Tspace &spc) : spc(spc) {
                        name="nonbonded";
                        pairpot = j;
                        Rc2_g2g = std::pow( j.value("cutoff_g2g", pc::infty), 2);
                    }

                    double energy(Change &change) override {
                        using namespace ranges;
                        double u=0;

                        if (!change.empty()) {

                            if (change.dV) {
#pragma omp parallel for reduction (+:u) schedule (dynamic) 
                                for ( auto i = spc.groups.begin(); i < spc.groups.end(); ++i ) {
                                    for ( auto j=i; ++j != spc.groups.end(); )
                                        u += g2g( *i, *j );
                                    if (i->atomic)
                                        u += g_internal(*i);
                                }
                                return u;
                            }

                            // did everything change?
                            if (change.all) {
#pragma omp parallel for reduction (+:u) schedule (dynamic) 
                                for ( auto i = spc.groups.begin(); i < spc.groups.end(); ++i ) {
                                    for ( auto j=i; ++j != spc.groups.end(); )
                                        u += g2g( *i, *j );
                                    u += g_internal(*i);
                                }
                                // more todo here...
                                return u;
                            }

                            // if exactly ONE molecule is changed
                            if (change.groups.size()==1) {
                                auto& d = change.groups[0];

                                // exactly ONE atom is changed
                                // WARNING! This does not respect inactive particles!
                                // TODO: Loop over groups instead
                                if (d.atoms.size()==1) {
                                    auto i = spc.groups[d.index].begin() + d.atoms[0];
                                    for (auto j=spc.p.begin(); j!=spc.p.end(); ++j)
                                        if (i!=j)
                                            u += i2i(*i, *j);
                                    return u;
                                }

                                // everything in group changed
                                if (d.all) {
#pragma omp parallel for reduction (+:u) 
                                    for (int i=0; i<int(spc.groups.size()); i++)
                                        if (i!=d.index)
                                            u+=g2g(spc.groups[i], spc.groups[d.index]);
                                    return u + g_internal(spc.groups[d.index]);
                                }
                            }

                            auto moved = change.touchedGroupIndex(); // index of moved groups
                            auto fixed = view::ints( 0, int(spc.groups.size()) )
                                | view::remove_if(
                                        [&moved](int i){return std::binary_search(moved.begin(), moved.end(), i);}
                                        ); // index of static groups

                            // moved<->moved
                            for ( auto i = moved.begin(); i != moved.end(); ++i )
                                for ( auto j=i; ++j != moved.end(); )
                                    u += g2g( spc.groups[*i], spc.groups[*j] );

                            // moved<->static
                            for ( auto i : moved)
                                for ( auto j : fixed) {
                                    u += g2g(spc.groups[i], spc.groups[j]);
                                }

                            // more todo!
                        }
                        return u;
                    }

            }; //!< Nonbonded, pair-wise additive energy term

        template<typename Tspace, typename Tpairpot>
            class NonbondedCached : public Nonbonded<Tspace,Tpairpot> {
                private:
                    typedef Nonbonded<Tspace,Tpairpot> base;
                    typedef typename Tspace::Tgroup Tgroup;

                    double g2g(const Tgroup &g1, const Tgroup &g2) override {
                        int i = &g1 - &base::spc.groups.front();
                        int j = &g2 - &base::spc.groups.front();
                        if (base::key==Energybase::NEW)        // if this is from the trial system,
                            cache.set(i, j, base::g2g(g1,g2)); // then update cache
                        return cache(i,j);                     // return (cached) value
                    }

                public:
                    PairMatrix<double> cache;
                    NonbondedCached(const json &j, Tspace &spc) : base(j,spc) {
                        base::name += "EM";
                        cache.resize( spc.groups.size() );
                    }

                    void sync(Energybase *basePtr, Change &change) override {
                        auto other = dynamic_cast<decltype(this)>(basePtr);
                        assert(other);
                        cache = other->cache;
                    } //!< Copy energy matrix from other
            }; //!< Nonbonded with cached energies (Energy Matrix)

        template<typename Tspace>
            class Penalty : public Energybase {
                private:
                    typedef typename Tspace::Tparticle Tparticle;
                    typedef typename Tspace::Tgroup Tgroup;
                    typedef typename Tspace::Tpvec Tpvec;

                    Tspace &spc;
                    std::shared_ptr<ReactionCoordinate::ReactionCoordinateBase> rc=nullptr;

                    Table<int> histo;
                    Table<double> penalty;

                public:
                    Penalty(const json &j, Tspace &spc) : spc(spc) {
                        using namespace ReactionCoordinate;
                        name = "penalty";
                        std::string type = j.at("type");
                        if (type=="cm")
                            rc = std::make_shared<MassCenterSeparation>(j, spc);
                        if (rc==nullptr)
                            throw std::runtime_error(name + ": unknown type'" + type + "'");
                        rc->name = type;

                        auto bw = j.value("binwidth", std::vector<double>({1.0, 1.0})); 

                        histo.reInitializer(bw, rc->min, rc->max);
                        penalty.reInitializer(bw, rc->min, rc->max);
                    }

                    void to_json(json &j) const override {
                        j = *rc;
                        j["type"] = rc->name;
                    }

                    double energy(Change &change) override {
                        double u=0;
                        if (!change.empty()) {
                            std::vector<double> coord = rc->operator()(); // obtain reaction coordinate
                            if (key==NEW)
                                cout << "trial state";
                            if (key==OLD)
                                cout << "old state";
                        }
                        return u;
                    }

                    void sync(Energybase *basePtr, Change &change) override {
                        // this is called upon rejection/acceptance and can be
                        // used to sync properties, if needed.
                        auto other = dynamic_cast<decltype(this)>(basePtr);
                        assert(other);
                        if (other->key==OLD)
                            cout << "move was rejected";
                        if (other->key==NEW)
                            cout << "move was accepted";
                    }
            };

#ifdef FAU_POWERSASA
        template<class Tspace>
            class SASAEnergy : public Energybase {
                private:
                    typedef typename Tspace::Tparticle Tparticle;
                    typedef typename Tspace::Tpvec Tpvec;
                    Tspace& spc;
                    std::vector<float> sasa, radii; 
                    std::vector<Point> coords;
                    double probe; // sasa probe radius (angstrom)
                    double conc=0;// co-solute concentration (mol/l)
                    Average<double> avgArea; // average surface area
                    std::shared_ptr<POWERSASA::PowerSasa<float,Point>> ps;

                    void updateSASA(const Tpvec &p) {
                        radii.resize(p.size());
                        coords.resize(p.size());
                        std::transform(p.begin(), p.end(), coords.begin(), [](auto &a){ return a.pos;});
                        std::transform(p.begin(), p.end(), radii.begin(),
                                [this](auto &a){ return atoms<Tparticle>[a.id].sigma*0.5 + this->probe;});

                        ps->update_coords(coords, radii); // slowest step!

                        for (size_t i=0; i<p.size(); i++) {
                            auto &a = atoms<Tparticle>[p[i].id];
                            if (std::fabs(a.tfe)>1e-9 || std::fabs(a.tension)>1e-9)
                                ps->calc_sasa_single(i);
                        }
                        sasa = ps->getSasa();
                        assert(sasa.size()==p.size());
                    }

                    void to_json(json &j) const override {
                        using namespace u8;
                        j["molarity"] = conc / 1.0_molar;
                        j["radius"] = probe / 1.0_angstrom;
                        j[bracket("SASA")+"/"+angstrom+squared] = avgArea.avg() / 1.0_angstrom;
                        _roundjson(j,5);
                    }

                public:
                    SASAEnergy(const json &j, Tspace &spc) : spc(spc) {
                        name = "sasa";
                        cite = "doi:10.1002/jcc.21844";
                        probe = j.value("radius", 1.4) * 1.0_angstrom;
                        conc = j.value("molarity", conc) * 1.0_molar;

                        radii.resize(spc.p.size());
                        coords.resize(spc.p.size());
                        std::transform(spc.p.begin(), spc.p.end(), coords.begin(), [](auto &a){ return a.pos;});
                        std::transform(spc.p.begin(), spc.p.end(), radii.begin(),
                                [this](auto &a){ return atoms<Tparticle>[a.id].sigma*0.5 + this->probe;});

                        ps = std::make_shared<POWERSASA::PowerSasa<float,Point>>(coords,radii);
                    }

                    double energy(Change &change) override {
                        double u=0, A=0;
                        updateSASA(spc.p);
                        for (size_t i=0; i<sasa.size(); ++i) {
                            auto &a = atoms<Tparticle>[ spc.p[i].id ];
                            u += sasa[i] * (a.tension + conc * a.tfe);
                            A += sasa[i];
                        }
                        avgArea+=A; // sample average area for accepted confs. only
                        return u;
                    }
            }; //!< SASA energy from transfer free energies
#endif

        template<typename Tspace>
            class Hamiltonian : public Energybase, public BasePointerVector<Energybase> {
                protected:
                    typedef typename Tspace::Tparticle Tparticle;
                    void to_json(json &j) const override {
                        for (auto i : this->vec)
                            j.push_back(*i);
                    }

                    void addEwald(const json &j, Tspace &spc) {
                        if (j.count("coulomb")==1)
                            if (j["coulomb"].at("type")=="ewald")
                                push_back<Energy::Ewald<Tspace>>(j["coulomb"], spc);
                    } //!< Adds an instance of reciprocal space Ewald energies (if appropriate)

                public:
                    Hamiltonian(Tspace &spc, const json &j) {
                        using namespace Potential;

                        typedef CombinedPairPotential<CoulombGalore,LennardJones<Tparticle>> CoulombLJ;
                        typedef CombinedPairPotential<CoulombGalore,HardSphere<Tparticle>> CoulombHS;
                        typedef CombinedPairPotential<CoulombGalore,WeeksChandlerAndersen<Tparticle>> CoulombWCA;
                        typedef CombinedPairPotential<Coulomb,WeeksChandlerAndersen<Tparticle>> PrimitiveModelWCA;

                        Energybase::name="hamiltonian";
                        for (auto &m : j.at("energy")) {// loop over move list
                            size_t oldsize = vec.size();
                            for (auto it=m.begin(); it!=m.end(); ++it) {
                                try {
                                    if (it.key()=="nonbonded_coulomblj")
                                        push_back<Energy::Nonbonded<Tspace,CoulombLJ>>(it.value(), spc);

                                    if (it.key()=="nonbonded_coulombhs")
                                        push_back<Energy::Nonbonded<Tspace,CoulombHS>>(it.value(), spc);

                                    if (it.key()=="nonbonded_coulombwca")
                                        push_back<Energy::Nonbonded<Tspace,CoulombWCA>>(it.value(), spc);

                                    if (it.key()=="nonbonded_pmwca")
                                        push_back<Energy::Nonbonded<Tspace,PrimitiveModelWCA>>(it.value(), spc);

                                    if (it.key()=="bonded")
                                        push_back<Energy::Bonded<Tspace>>(it.value(), spc);

                                    if (it.key()=="confine")
                                        push_back<Energy::Confine<Tspace>>(it.value(), spc);

                                    if (it.key()=="isobaric")
                                        push_back<Energy::Isobaric<Tspace>>(it.value(), spc);

                                    if (it.key()=="penalty")
                                        push_back<Energy::Penalty<Tspace>>(it.value(), spc);
#ifdef ENABLE_POWERSASA
                                    if (it.key()=="sasa")
                                        push_back<Energy::SASAEnergy<Tspace>>(it.value(), spc);
#endif

                                    // additional energies go here...

                                    addEwald(it.value(), spc); // add reciprocal Ewald terms if appropriate

                                    if (vec.size()==oldsize)
                                        std::cerr << "warning: ignoring unknown energy '" << it.key() << "'" << endl;

                                } catch (std::exception &e) {
                                    throw std::runtime_error("Error adding energy '" + it.key() + "': " + e.what());
                                }
                            }
                        }
                    }

                    double energy(Change &change) override {
                        double du=0;
                        for (auto i : this->vec) {
                            i->key=key;
                            du += i->energy(change);
                        }
                        return du;
                    } //!< Energy due to changes

                    void sync(Hamiltonian &other, Change &change) {
                        assert(other.size()==size());
                        for (size_t i=0; i<size(); i++)
                            this->vec[i]->sync( other.vec[i].get(), change);
                    }

            }; //!< Aggregates and sum energy terms

    }//namespace
}//namespace
