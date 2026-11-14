"""Paired-market inference; markets, not individual fills, are sampling units."""
import numpy as np
from scipy.stats import binomtest,t

def paired(variant,base,seed=7219):
    x=np.asarray(variant,dtype=float);y=np.asarray(base,dtype=float)
    if x.shape!=y.shape or x.ndim!=1 or len(x)<2 or not np.isfinite(x).all() or not np.isfinite(y).all():
        raise ValueError('Need finite paired vectors of equal length, n>=2')
    d=x-y;n=len(d);mean=float(d.mean());se=float(d.std(ddof=1)/np.sqrt(n))
    wins=int((d>0).sum());losses=int((d<0).sum());nonzero=wins+losses
    p=float(binomtest(wins,nonzero,.5).pvalue) if nonzero else 1.
    half=float(t.ppf(.975,n-1)*se)
    rng=np.random.default_rng(seed);boot=d[rng.integers(0,n,(10000,n))].mean(axis=1)
    lo,hi=np.quantile(boot,[.025,.975])
    return dict(n=n,mean_difference=mean,se=se,t_ci_low=mean-half,t_ci_high=mean+half,bootstrap_ci_low=float(lo),bootstrap_ci_high=float(hi),wins=wins,losses=losses,ties=n-nonzero,sign_p=p)

def holm(pvalues):
    p=np.asarray(pvalues,dtype=float)
    if p.ndim!=1 or not np.isfinite(p).all() or np.any((p<0)|(p>1)):raise ValueError('Invalid p values')
    order=np.argsort(p);result=np.empty(len(p));running=0.
    for rank,i in enumerate(order):
        running=max(running,(len(p)-rank)*p[i]);result[i]=min(1.,running)
    return result
