#include "analysis.h"

void Faunus::Analysis::to_json(Faunus::json &j, const Faunus::Analysis::Analysisbase &base) {
    base.to_json( j );
}

Faunus::Analysis::Analysisbase::~Analysisbase() {}

void Faunus::Analysis::Analysisbase::sample() {
    stepcnt++;
    if ( stepcnt == steps ) {
        cnt++;
        stepcnt = 0;
        timer.start();
        _sample();
        timer.stop();
    }
}

void Faunus::Analysis::Analysisbase::from_json(const Faunus::json &j) {
    steps = j.value("nstep", 0);
    _from_json(j);
}

void Faunus::Analysis::Analysisbase::to_json(Faunus::json &j) const {
    assert( !name.empty() );
    auto &_j = j[name];
    _to_json(_j);
    if (cnt>0) {
        _j["relative time"] = _round( timer.result() );
        _j["nstep"] = steps;
        _j["samples"] = cnt;
    }
    if (!cite.empty())
        _j["reference"] = cite;
}

void Faunus::Analysis::Analysisbase::_to_json(Faunus::json &j) const {}

void Faunus::Analysis::Analysisbase::_from_json(const Faunus::json &j) {}

void Faunus::Analysis::SystemEnergy::normalize() {
    //assert(V.cnt>0);
    double Vr=1, sum = ehist.sumy();
    for (auto &i : ehist.getMap()) {
        i.second = i.second/sum ;
    }
}

void Faunus::Analysis::SystemEnergy::_sample() {
    auto ulist = energyFunc();
    double tot = std::accumulate(ulist.begin(), ulist.end(), 0.0);
    if (not std::isinf(tot))
        uavg+=tot;
    f << cnt*steps << sep << tot;
    for (auto u : ulist)
        f << sep << u;
    f << "\n";
    //ehist(tot)++;
}

void Faunus::Analysis::SystemEnergy::_to_json(Faunus::json &j) const {
    j = { {"file", file}, {"init",uinit}, {"final", energyFunc()} };
    if (cnt>0)
        j["mean"] = uavg.avg();
    _roundjson(j,5);
    //normalize();
    //ehist.save( "distofstates.dat" );

}

void Faunus::Analysis::SystemEnergy::_from_json(const Faunus::json &j) {
    file = MPI::prefix + j.at("file").get<std::string>();
    if (f)
        f.close();
    f.open(file);
    if (!f)
        throw std::runtime_error(name + ": cannot open output file " + file);
    assert(!names.empty());
    std::string suffix = file.substr(file.find_last_of(".") + 1);
    if (suffix=="csv")
        sep=",";
    else {
        sep=" ";
        f << "#";
    }
    f << "total";
    for (auto &n : names)
        f << sep << n;
    f << "\n";
}

void Faunus::Analysis::SaveState::_to_json(Faunus::json &j) const {
    j["file"] = file;
}

void Faunus::Analysis::SaveState::_sample() {
    writeFunc(file);
}

Faunus::Analysis::SaveState::~SaveState() {
    if (steps==-1)
        _sample();
}

Faunus::Analysis::PairFunctionBase::PairFunctionBase(const Faunus::json &j) { from_json(j); }

Faunus::Analysis::PairFunctionBase::~PairFunctionBase() {
    normalize();
    hist.save( MPI::prefix + file );
}

void Faunus::Analysis::PairFunctionBase::normalize() {
    //assert(V.cnt>0);
    double Vr=1, sum = hist.sumy();
    for (auto &i : hist.getMap()) {
        if (dim==3)
            Vr = 4 * pc::pi * std::pow(i.first,2) * dr;
        if (dim==2) {
            Vr = 2 * pc::pi * i.first * dr;
            if ( Rhypersphere > 0)
                Vr = 2.0*pc::pi*Rhypersphere*std::sin(i.first/Rhypersphere) * dr;
        }
        if (dim==1)
            Vr = dr;
        i.second = i.second/sum * V/Vr;
    }
}

void Faunus::Analysis::PairFunctionBase::_to_json(Faunus::json &j) const {
    j = {
        {"dr", dr/1.0_angstrom},
        {"name1", name1},
        {"name2", name2},
        {"file", file},
        {"dim", dim}
    };
    if (Rhypersphere>0)
        j["Rhyper"] = Rhypersphere;
}

void Faunus::Analysis::PairFunctionBase::_from_json(const Faunus::json &j) {
    file = j.at("file");
    name1 = j.at("name1");
    name2 = j.at("name2");
    dim = j.value("dim", 3);
    dr = j.value("dr", 0.1) * 1.0_angstrom;
    hist.setResolution(dr);
    hist2.setResolution(dr);
    Rhypersphere = j.value("Rhyper", -1.0);
}

void Faunus::Analysis::VirtualVolume::_sample() {
    if (fabs(dV)>1e-10) {
        double Vold = getVolume(), Uold = pot.energy(c);
        scaleVolume(Vold + dV);
        double Unew = pot.energy(c);
        scaleVolume(Vold);
        duexp += exp(-(Unew - Uold));
        assert(std::fabs((Uold-pot.energy(c))/Uold) < 1e-6);
    }
}

void Faunus::Analysis::VirtualVolume::_from_json(const Faunus::json &j) { dV = j.at("dV"); }

void Faunus::Analysis::VirtualVolume::_to_json(Faunus::json &j) const {
    double pex = log(duexp.avg()) / dV; // excess pressure
    j = {
        {"dV", dV}, {"Pex/mM", pex/1.0_mM},
        {"Pex/Pa", pex/1.0_Pa}, {"Pex/kT/"+u8::angstrom+u8::cubed, pex}
    };
    _roundjson(j,5);
}
