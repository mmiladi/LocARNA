#include <fstream>
#include <sstream>
#include <map>

#include "aux.hh"
#include "rna_data.hh"

#include "alphabet.hh"

#include "multiple_alignment.hh"

#include "basepairs.hh"

#ifdef HAVE_LIBRNA
extern "C" {
#include <ViennaRNA/fold_vars.h>
#include <ViennaRNA/part_func.h>
#include <ViennaRNA/fold.h>
#include <ViennaRNA/utils.h>
#include <ViennaRNA/energy_const.h>
#include <ViennaRNA/loop_energies.h>
#include <ViennaRNA/params.h>
#include <ViennaRNA/pair_mat.h>
}
#endif // HAVE_LIBRNA

#include <math.h>
#include <string.h>

namespace LocARNA {

    RnaData::RnaData(const std::string &file, bool stacking_):
	sequence(),
	stacking(stacking_),
	arc_probs_(0),
	arc_2_probs_(0),
	seq_constraints_(""),
	McC_matrices(NULL)
    {
	read(file);
    }
    
#ifdef HAVE_LIBRNA

    FLT_OR_DBL *qm1;
    FLT_OR_DBL *qm2;
    FLT_OR_DBL *scale_p;
    pf_paramT *pf_params_p;
    FLT_OR_DBL *expMLbase_p;
    RnaData::RnaData(const Sequence &sequence_, bool keepMcM)
	: sequence(sequence_),
	  stacking(false),
	  arc_probs_(0),
	  arc_2_probs_(0),
	  seq_constraints_(""),
	  McC_matrices(NULL)
    {
	if (sequence.row_number()!=1) {
	    std::cerr << "Construction with multi-row Sequence object is not implemented." << std::endl;
	    exit(-1);
	}
	
	// run McCaskill and get access to results
	// in McCaskill_matrices
	compute_McCaskill_matrices();
	
	// initialize the object from base pair probabilities
	// Use the same proability threshold as in RNAfold -p !
	init_from_McCaskill_bppm();
	
	// optionally deallocate McCaskill matrices
	if (!keepMcM) {
	    free_McCaskill_matrices();
	}
    }
    
    void
    RnaData::compute_McCaskill_matrices() {	
	if (sequence.row_number()!=1) {
	    std::cerr << "McCaskill computation with multi-row Sequence object is not implemented." << std::endl;
	    exit(-1);
	}
	
	assert(sequence.row_number()==1);
	
	// use MultipleAlignment to get pointer to c-string of the
	// first (and only) sequence in object sequence.
	//
	size_t length = sequence.length();
	
	char c_sequence[length+1];
	std::string seqstring = MultipleAlignment(sequence).seqentry(0).seq().to_string();
	strcpy(c_sequence,seqstring.c_str());
	
	char c_structure[length+1];
	
	
	// std::cout <<"Call fold(" << c_sequence << "," << "c_structure" << ")"<< std::endl;
	
	// call fold for setting the pf_scale
	
	double en = fold(c_sequence,c_structure);
	// std::cout << c_structure << std::endl;
	free_arrays();
	
	// set pf_scale
	double kT = (temperature+273.15)*1.98717/1000.;  /* kT in kcal/mol */
	pf_scale = exp(-en/kT/length);
	
	// std::cout <<"Call pf_fold(" << c_sequence << "," << "NULL" << ")"<< std::endl;
	
	// call pf_fold
	pf_fold(c_sequence,c_structure);
	
	McC_matrices = new McC_matrices_t();
	
	// get pointers to McCaskill matrices
	get_pf_arrays(&McC_matrices->S_p,
		      &McC_matrices->S1_p,
		      &McC_matrices->ptype_p,
		      &McC_matrices->qb_p,
		      &McC_matrices->qm_p,
		      &McC_matrices->q1k_p,
		      &McC_matrices->qln_p);
	// get pointer to McCaskill base pair probabilities
	McC_matrices->bppm = export_bppm();
	
	McC_matrices->iindx = get_iindx(sequence.length());
	pf_params_p= get_scaled_pf_parameters();
	expMLbase_p= (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(length+1));
	scale_p= (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(length+1));
	
	kT = pf_params_p->kT;   /* kT in cal/mol  */

	/* scaling factors (to avoid overflows) */
	if (pf_scale == -1) { /* mean energy for random sequences: 184.3*length cal */
	  pf_scale = exp(-(-185+(pf_params_p->temperature-37.)*7.27)/kT);
	  if (pf_scale<1) pf_scale=1;
	}
	scale_p[0] = 1.;
	scale_p[1] = 1./pf_scale;
	expMLbase_p[0] = 1;
	expMLbase_p[1] = pf_params_p->expMLbase/pf_scale;
	for (size_t i=2; i<=sequence.length(); i++) {
	  scale_p[i] = scale_p[i/2]*scale_p[i-(i/2)];
	  expMLbase_p[i] = pow(pf_params_p->expMLbase, (double)i) * scale_p[i];
	}
	
	compute_Qm2();
    
    }

    void
    RnaData::free_McCaskill_matrices() {
	// call respective librna function
	if (McC_matrices) {
	    free_pf_arrays();
	    delete McC_matrices;
	}
    }
    
#endif // HAVE_LIBRNA

    RnaData::~RnaData() {
#     ifdef HAVE_LIBRNA	
	free_McCaskill_matrices();
#     endif
    }

    
    // decide on file format and call either readPS or readPP
    void RnaData::read(const std::string &filename) {
  
	std::ifstream in(filename.c_str());
	if (! in.good()) {
	    std::cerr << "Cannot read "<<filename<<std::endl;
	    exit(-1);
	}
    
	std::string s;
	// read first line and decide about file-format
	in >> s;
	in.close();
	if (s == "%!PS-Adobe-3.0") {
	    // try reading as dot.ps file (as generated by RNAfold)
	    readPS(filename);
	    
	    //} else if (s == "<ppml>") {
	    // proprietary PPML file format
	    //readPPML(filename);

#ifdef HAVE_LIBRNA
	} else if (s.substr(0,7) == "CLUSTAL" || s[0]=='>') {
	    // assume multiple alignment format: read and compute base pair probabilities
	    readMultipleAlignment(filename, false);
#endif
	} else {
	    // try reading as PP-file (proprietary format, which is easy to read and contains pair probs)
	    readPP(filename);
	}

    
	// DUMP for debugging
	//std::cout << arc_probs_ << std::endl;
	//std::cout << arc_2_probs_ << std::endl;

    }

    void RnaData::readPS(const std::string &filename) {
	std::ifstream in(filename.c_str());
    
	bool contains_stacking_probs=false; // does the file contain probabilities for stacking
    
	std::string s;
	while (in >> s && s!="/sequence") {
	    if (s=="stacked") contains_stacking_probs=true;
	}
    
	if (stacking && ! contains_stacking_probs) {
	    std::cerr << "WARNING: Stacking requested, but no stacking probabilities in dot plot!" << std::endl;
	}

    
	in >> s; in >> s;

	std::string seqstr="";
	while (in >> s && s!=")") {
	    s = s.substr(0,s.size()-1); // chop of last character
	    // cout << s <<endl;
	    seqstr+=s;
	}
    
	std::string seqname = seqname_from_filename(filename);
    
	//! sequence characters should be upper case, and 
	//! Ts translated to Us
	normalize_rna_sequence(seqstr);
        
	sequence.append_row(seqname,seqstr);
            
	std::string line;
    
	while (getline(in,line)) {
	    if (line.length()>4) {
		std::string type=line.substr(line.length()-4);
		if (type == "ubox"
		    ||
		    type == "lbox"
		    ) {
		
		    std::istringstream ss(line);
		    unsigned int i,j;
		    double p;
		    ss >> i >> j >> p;
		
		    p*=p;
		
		    //std::cout << i << " " << j << std::endl;
		
		    if (! (1<=i && i<j && j<=sequence.length())) {
			std::cerr << "WARNING: Input dotplot "<<filename<<" contains invalid line " << line << " (indices out of range)" << std::endl;
			//exit(-1);
		    } else {
			if (type=="ubox") {
			    set_arc_prob(i,j,p);
			}
			else if (stacking && contains_stacking_probs && type=="lbox") { // read a stacking probability
			    set_arc_2_prob(i,j,p); // we store the joint probability of (i,j) and (i+1,j-1)
			}
		    }
		}
	    }
	}
    }

#ifdef HAVE_LIBRNA
    //bool flag;
    void RnaData::readMultipleAlignment(const std::string &filename, bool keepMcM) {
	
	//read to multiple alignment object
	MultipleAlignment ma(filename,MultipleAlignment::CLUSTAL); // accept clustal input
	// MultipleAlignment ma(filename,MultipleAlignment::FASTA); // accept fasta input
	
	// convert to sequence
	sequence = Sequence(ma);
	
	if (sequence.row_number()!=1) {
	    std::cerr << "ERROR: Cannot handle input from "<<filename<<"."<<std::endl
		      <<"        Base pair computation from multi-fasta is not implemented." << std::endl;
	    exit(-1);
	}
	
	// run McCaskill and get access to results
	// in McCaskill_matrices
	compute_McCaskill_matrices();
	
	// initialize the object from base pair probabilities
	// Use the same proability threshold as in RNAfold -p !
	init_from_McCaskill_bppm();
	
	if (!keepMcM) {
	    free_McCaskill_matrices();
	}
	
    }

    void
    RnaData::init_from_McCaskill_bppm(double threshold) {
	for( size_t i=1; i <= sequence.length(); i++ ) {
	    for( size_t j=i+1; j <= sequence.length(); j++ ) {
		
		double p=McC_matrices->get_bppm(i,j);
		
		if (p >= threshold) { // apply very relaxed filter 
		    set_arc_prob(i,j,p);
		}
	    }
	}
    }
    void
    RnaData::compute_Qm2(){
      
      int len= sequence.length();
      qm1= (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL)*(len+2));
      qm2= (FLT_OR_DBL *) space(sizeof(FLT_OR_DBL) * ((len+1)*(len+2)/2));
      FLT_OR_DBL *qqm1= (FLT_OR_DBL*) space(sizeof(FLT_OR_DBL)*(len+2));
      
      int index_i,index_j,index_k,type;
      for (index_i=1; index_i<=len; index_i++)
	qm1[index_i]=qqm1[index_i]=0;
     
      for(index_j= TURN+2; index_j<=len; index_j++){
	for(index_i= index_j-TURN-1; index_i>=1; index_i--){
	 type= McC_matrices->ptype_p[iindx[index_i]-index_j];
	 qm1[index_i]= qqm1[index_i]*expMLbase_p[1];
	 if(type){
	  qm1[index_i]+= (McC_matrices->qb_p[iindx[index_i]-index_j])* exp_E_MLstem(type, (index_i>1) ? 
									McC_matrices->S1_p[index_i-1] : -1, (index_j<len) ? McC_matrices->S1_p[index_j+1] : -1,  pf_params_p);

	  }
	}
	if(index_j >= (2*(TURN+2))){
	  for(index_i= index_j-2*TURN-3; index_i>=1; index_i--){
	    qm2[iindx[index_i]-index_j]= 0;
	      for(index_k= index_i+2; index_k< index_j-2; index_k++){
		qm2[iindx[index_i+1]-(index_j-1)]+= McC_matrices->qm_p[iindx[index_i+1]-(index_k)]*qqm1[index_k+1];
	      
	      }
	  }
	}
	  for(index_i= index_j-TURN-1; index_i>=1; index_i--){
	    qqm1[index_i]=qm1[index_i];
	  }
	}

      
    }
    double RnaData::prob_unpaired_in_loop(size_type k,size_type i,size_type j){
	
	int length = sequence.length();
	
	char c_sequence[length+1];
	std::string seqstring = MultipleAlignment(sequence).seqentry(0).seq().to_string();
	strcpy(c_sequence,seqstring.c_str());
	
	FLT_OR_DBL H,I,M;
	int type,type_2;
	type= McC_matrices->ptype_p[iindx[i]-j];
	if (type!=0) {
	  if (((type==3)||(type==4))&&no_closingGU) H = 0;
	  else
	  H = exp_E_Hairpin(j-i-1, type, McC_matrices->S1_p[i+1], McC_matrices->S1_p[j-1], c_sequence+i-1, pf_params_p) * scale_p[j-i+1];
	}
	else H= 0;
	I= 0.0;
	if(type!=0){
	  int u1;
	  for (int i_p=k+1; i_p<=MIN2(i+MAXLOOP+1,j-TURN-2); i_p++) {
	    u1 = i_p-i-1;
	    for (int j_p=MAX2(i_p+TURN+1,j-1-MAXLOOP+u1); j_p<(int)j; j_p++) {
	      type_2 =McC_matrices-> ptype_p[iindx[i_p]-j_p];
	      if (type_2) {
		type_2 = rtype[type_2];
		I +=McC_matrices-> qb_p[iindx[i_p]-j_p] * (scale_p[u1+j-j_p+1] *
                                        exp_E_IntLoop(u1, j-j_p-1, type, type_2,
                                       McC_matrices-> S1_p[i+1],McC_matrices-> S1_p[j-1],McC_matrices-> S1_p[i_p-1],McC_matrices-> S1_p[j_p+1], pf_params_p));
	      }
	    }
	  }
	  
	  for (int i_p=i+1; i_p<=MIN2(i+MAXLOOP+1,k-TURN-2); i_p++) {
	    u1 = i_p-i-1;
	    for (int j_p=MAX2(i_p+TURN+1,j-1-MAXLOOP+u1); j_p<(int)k; j_p++) {
	      type_2 =McC_matrices-> ptype_p[iindx[i_p]-j_p];
	      if (type_2) {
		type_2 = rtype[type_2];
		I +=McC_matrices-> qb_p[iindx[i_p]-j_p] * (scale_p[u1+j-j_p+1] *
                                        exp_E_IntLoop(u1, j-j_p-1, type, type_2,
                                       McC_matrices-> S1_p[i+1],McC_matrices-> S1_p[j-1],McC_matrices-> S1_p[i_p-1],McC_matrices-> S1_p[j_p+1], pf_params_p));
	      }
	    }
	  }
	  
	}
	
	M= 0.0;
	if(type!=0){
		M+= qm2[iindx[k+1]-(j-1)]*pf_params_p->expMLclosing*expMLbase_p[k-i]*exp_E_MLstem(rtype[type],McC_matrices-> S1_p[j-1],McC_matrices-> S1_p[i+1], pf_params_p)* scale_p[2];
		M+= qm2[iindx[i+1]-(k-1)]*pf_params_p->expMLclosing*expMLbase_p[j-k]*exp_E_MLstem(rtype[type],McC_matrices-> S1_p[j-1],McC_matrices-> S1_p[i+1], pf_params_p) * scale_p[2];
		M+= McC_matrices->qm_p[iindx[i+1]-(k-1)] * McC_matrices->qm_p[iindx[k+1]-(j-1)]* pf_params_p->expMLclosing*expMLbase_p[1] *exp_E_MLstem(rtype[type],McC_matrices-> S1_p[j-1],McC_matrices-> S1_p[i+1], pf_params_p)* scale_p[2];
	  }
	return (McC_matrices->qb_p[iindx[i]-j]==0)? 0: ((H+I+M)/(McC_matrices->qb_p[iindx[i]-j]))*get_arc_prob(i,j);
    }

#endif // HAVE_LIBRNA

    void RnaData::readPP(const std::string &filename) {
	std::ifstream in(filename.c_str());
    
	std::string name;
	std::string seqstr;
    
    
	// ----------------------------------------
	// read sequence/alignment
    
	std::map<std::string,std::string> seq_map;
    
	std::string line;
    
	while (getline(in,line) && line!="#" ) {
	    if (line.length()>0 && line[0]!=' ') {
		std::istringstream in(line);
		in >> name >> seqstr;
	    
		normalize_rna_sequence(seqstr);
	    
		if (name != "SCORE:") { // ignore the (usually first) line that begins with SCORE:
		    if (name == "#C") {
			seq_constraints_ += seqstr;
		    } else {
			seq_map[name] += seqstr;
		    }
		}
	    }
	}
    
	for (std::map<std::string,std::string>::iterator it=seq_map.begin(); it!=seq_map.end(); ++it) {
	    // std::cout << "SEQ: " << it->first << " " << it->second << std::endl;
	    sequence.append_row(it->first,it->second);
	}
    
	// ----------------------------------------
	// read base pairs
    
	int i,j;
	double p;

	// std::cout << "LEN: " << len<<std::endl;
    
	while( getline(in,line) ) {
	    std::istringstream in(line);
      
	    in>>i>>j>>p;
      
	    if ( in.fail() ) continue; // skip lines that do not specify a base pair probability
      
	    if (i>=j) {
		std::cerr << "Error in PP input line \""<<line<<"\" (i>=j).\n"<<std::endl;
		exit(-1);
	    }
      
	    set_arc_prob(i,j,p);
      
	    if (stacking) {
		double p2;
	  
		if (in >> p2) {
		    set_arc_2_prob(i,j,p2); // p2 is joint prob of (i,j) and (i+1,j-1)
		}
	    }      
	}
    }

	

    /*
      void RnaData::readPPML(const std::string &filename) {

      std::ifstream in(filename.c_str());
    
      std::string tag;
      while (in>>tag) {
      if (tag == "<score>") {
      readScore(in);
      } else if  (tag == "<alignment>") {
      readAlignment(in);
      } else if(tag == "<bpp>") {
      readBPP(in);
      } else if(tag == "<constraints>") {
      readConstraints(in);
      }
      }

      std::string name;
      std::string seqstr;
    
      }
    */

    std::string RnaData::seqname_from_filename(const std::string &s) const {
	size_type i;
	size_type j;
    
	assert(s.length()>0);
    
	for (i=s.length(); i>0 && s[i-1]!='/'; i--)
	    ;

	for (j=i; j<s.length() && s[j]!='.'; j++)
	    ;

	std::string name=s.substr(i,j-i);

	if (name.length()>3 && name.substr(name.length()-3,3) == "_dp")
	    name=name.substr(0,name.length()-3);
    
	return name;
    }

}
