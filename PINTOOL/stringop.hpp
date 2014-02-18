// SIMULATE
template<UINT32 lengthInBits> void STRINGOP::sMOVS
    (ADDRINT count, ADDRINT flagsValue, ADDRINT readAddress, ADDRINT writeAddress ADDRESS_DEBUG) 
{
    // copie de "count" octets/mot/double mots de [ESI] vers [EDI] (idem MOV mem->mem)
    ADDRINT nbIterations = count * (lengthInBits >> 3); // nombre d'octets copi�s
    
    // direction Flag mis � 1 : les adresses vont d�croissantes, sinon ca sera l'inverse
    UINT32 isDecrease = (flagsValue >> DIRECTION_FLAG) & 1; 
    
    for (UINT32 counter = 0 ; counter < nbIterations; ++counter)
    {
        if (pTmgrGlobal->isMemoryTainted<8>(readAddress))	
        {
            _LOGTAINT("MOVS " << lengthInBits<< "bits");
            pTmgrGlobal->updateMemoryTaint<8>(writeAddress, std::make_shared<TaintByte>(
                X_ASSIGN,
                ObjectSource(pTmgrGlobal->getMemoryTaint<8>(readAddress))));
        }
        else pTmgrGlobal->unTaintMemory<8>(writeAddress);
            
        if (isDecrease)
        {
            --readAddress; --writeAddress;
        }
        else
        {
            ++readAddress; ++writeAddress;
        }
    }
} //sMOVS

template<UINT32 lengthInBits> void STRINGOP::sLODS
    (THREADID tid, ADDRINT count, ADDRINT flagsValue, ADDRINT readAddress ADDRESS_DEBUG) 
{
    // d�placement de "count" octets/mot/dble mots de [ESI] vers AL/AX/EAX/RAX
    // en r�alit�, seule la derni�re it�ration nous interesse
    // ce sera un MOV_MR<lengthInBits> de lastAddress vers AL/AX/EAX/RAX
  
    // calcul de l'adresse point�e par ESI lors de la derni�re op�ration
    // DF a 1 => adresses d�croissantes, DF a 0 => adresses croissantes
    ADDRINT lastAddress = ((flagsValue >> DIRECTION_FLAG) & 1) 
        ? (readAddress - ((count-1) * (lengthInBits >> 3))) 
        : (readAddress + ((count-1) * (lengthInBits >> 3))); 
    
    // appel de la proc�dure MOVMR ad�quate selon le registre
    _LOGTAINT("LODS (si marqu� : message movRM suivra)");
    DATAXFER::sMOV_MR<lengthInBits>(tid, lastAddress, registerACC<lengthInBits>::getReg() INSADDRESS);
} // sLODS

template<UINT32 lengthInBits> void STRINGOP::sSTOS
    (THREADID tid, ADDRINT count, ADDRINT flagsValue, ADDRINT writeAddress ADDRESS_DEBUG) 
{
    // d�placement de "count" octets/mot/dble mots de AL/AX/EAX/RAX -> [EDI] 
    
    TaintManager_Thread *pTmgrTls = static_cast<TaintManager_Thread*>(PIN_GetThreadData(g_tlsKeyTaint, tid));
        
    // 1) recup�ration du marquage du registre, et stockage dans un vecteur.
    // et stockage dans un tableau d'objets (ou nullPtr)
    vector<TaintBytePtr> vTaintRegSrc;
    for (UINT32 regPart = 0 ; regPart < (lengthInBits >> 3) ; ++regPart) 
    {
        #if TARGET_IA32
        vTaintRegSrc.push_back(pTmgrTls->getRegisterPartTaint(regIndexEAX, regPart));
        #else
        vTaintRegSrc.push_back(pTmgrTls->getRegisterPartTaint(regIndexRAX, regPart));
        #endif  
    }

    // si DF = 1 => adresses d�croissantes, sinon le contraire
    UINT32 isDecrease = (flagsValue >> DIRECTION_FLAG) & 1; 

    for (UINT32 iteration = 0 ; iteration < count ; ++iteration) // r�p�tition "count" fois
    {
        // marquage de chaque plage de 'lengthInBits' octets avec le registre source     
        for (auto it = vTaintRegSrc.begin(); it != vTaintRegSrc.end() ; ++it) 
        {
            if ((bool) *it)
            {
                pTmgrGlobal->updateMemoryTaint<8>(writeAddress, std::make_shared<TaintByte>(
                    X_ASSIGN,
                    ObjectSource(*it)));
            }
            else pTmgrGlobal->unTaintMemory<8>(writeAddress);

            // calcul de la prochaine adresse
            (isDecrease) ? --writeAddress : ++writeAddress;
        }
    }
} // sSTOS

template<UINT32 lengthInBits> void STRINGOP::sStoreTaintSCAS(
    THREADID tid, BOOL isREPZ, ADDRINT regValue, ADDRINT insAddress) 
{ 
    // proc�dure appel�e lors de la premi�re it�ration (callback IF/THEN)
    // sert � stocker dans TaintManager les arguments (marquage, adresse)
    
    TaintManager_Thread *pTmgrTls = static_cast<TaintManager_Thread*>(PIN_GetThreadData(g_tlsKeyTaint, tid));
    StringOpInfo strInfo = {0};

    // mise en cache de la valeur du pr�fixe (true = REPZ/E, false = REPNZ/E)
    strInfo.isREPZ = isREPZ; 
    
    // si registre marqu�, stockage de l'objet, sinon mise � nullptr 
    // et stockage de la valeur
    if (pTmgrTls->isRegisterTainted<lengthInBits>(registerACC<lengthInBits>::getReg())) 
    {
        strInfo.isRegTainted = true;
        strInfo.tPtr = pTmgrTls->getRegisterTaint<lengthInBits>(registerACC<lengthInBits>::getReg(), regValue);
    }
    else 
    {
        strInfo.isRegTainted = false;
        strInfo.tPtr = nullptr;	 
        strInfo.regValue = regValue; // stockage de la valeur du registre
    }
    // mise en cache
    pTmgrTls->storeStringOpInfo(strInfo);
} // storeTaintSCAS

template<UINT32 lengthInBits> void STRINGOP::sSCAS(THREADID tid, ADDRINT address) 
{
    // comparaison de AL/AX/EAX/RAX avec [EDI] 
    // + ajout d'une contrainte sur ZFLAG, indiquant une r�p�tition ou non
    // m�tohde identique � CMP_MR, reprise ici int�gralement afin d'ins�rer la contrainte sur le ZFLAG
    
    TaintManager_Thread *pTmgrTls = static_cast<TaintManager_Thread*>(PIN_GetThreadData(g_tlsKeyTaint, tid));
    StringOpInfo strInfo = pTmgrTls->getStoredStringOpInfo();
    
    bool isSrcDestTainted = strInfo.isRegTainted;
    bool isSrcTainted =		pTmgrGlobal->isMemoryTainted<lengthInBits>(address);

    if ( !(isSrcDestTainted || isSrcTainted))   pTmgrTls->unTaintAllFlags();
    else 
    {
        ADDRINT insAddress = strInfo.instrPtr; // adresse de l'ins (en cache)
        _LOGTAINT("SCAS");
        
        // calcul des sources pour le futur objet
        ObjectSource srcDest = (isSrcDestTainted) 
            ? ObjectSource(strInfo.tPtr) : ObjectSource(lengthInBits, strInfo.regValue);
        
        ObjectSource src = (isSrcTainted) 
            ? ObjectSource(pTmgrGlobal->getMemoryTaint<lengthInBits>(address))
            : ObjectSource(lengthInBits, getMemoryValue<lengthInBits>(address));

        // marquage flags 
        BINARY::fTaintCMP<lengthInBits>(pTmgrTls, srcDest, src);
        // d�claration de la contrainte
        g_pFormula->addConstraint_ZERO(pTmgrTls, insAddress, strInfo.isREPZ);
    }
} // sSCAS

template<UINT32 lengthInBits> void STRINGOP::sCMPS
    (THREADID tid, UINT32 repCode, ADDRINT esiAddr, ADDRINT ediAddr, ADDRINT insAddress) 
{
    bool isSrcTainted =		pTmgrGlobal->isMemoryTainted<lengthInBits>(esiAddr);
    bool isSrcDestTainted = pTmgrGlobal->isMemoryTainted<lengthInBits>(ediAddr);	
    
    TaintManager_Thread *pTmgrTls = static_cast<TaintManager_Thread*>(PIN_GetThreadData(g_tlsKeyTaint, tid));
    
    if ( !(isSrcDestTainted || isSrcTainted)) pTmgrTls->unTaintAllFlags();
    else 
    {
        _LOGTAINT("CMPS");
        
        ObjectSource src = (isSrcTainted) 
            ? ObjectSource(pTmgrGlobal->getMemoryTaint<lengthInBits>(esiAddr))
            : ObjectSource(lengthInBits, getMemoryValue<lengthInBits>(esiAddr));
       
        ObjectSource srcDest = (isSrcDestTainted) 
            ? ObjectSource(pTmgrGlobal->getMemoryTaint<lengthInBits>(ediAddr))
            : ObjectSource(lengthInBits, getMemoryValue<lengthInBits>(ediAddr));

        // marquage flags (resultat calcul� dans la proc�dure Flags) 
        BINARY::fTaintCMP<lengthInBits>(pTmgrTls, srcDest, src);	
        
        // d�claration de la contrainte s'il y avait un pr�fixe REP (codes 1 REPE = 0 et 2)
        if (repCode)  g_pFormula->addConstraint_ZERO(pTmgrTls, insAddress, (repCode == 1) ? true : false); 
    }
} // sCMPS