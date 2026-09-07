# Adapted from https://github.com/sogaiu/jref
# This also includes the license from that project

# Copyright (c) 2019, 2020, 2021, 2022, 2023, 2024, 2025 Calvin Rose and
# contributors

# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
# of the Software, and to permit persons to whom the Software is furnished to do
# so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
(import /build/jit :as jit)

(defn jit-result-deep-matches [f & args]
  (let [jit-f (jit/jitable f :error)]
    (let [expected (f ;args)
	  actual (jit-f ;args)]
      (do
	(if (not (jit/compiled? jit-f))
	  (error "function did not compile"))
	(if (or (deep= expected actual) (and (nan? expected) (nan? actual)))
	  (do
	    true)
	  (do
	    (print "x (deep) expected " expected " actual: " actual " args: " (string/format "%p" args))
	    (pp (disasm f))
	    false))))))

(defn jit-result-matches [f & args]
  (let [jit-f (jit/jitable f :error)]
    (let [expected (f ;args)
	  actual (jit-f ;args)]
      (do
	(if (not (jit/compiled? jit-f))
	  (error "function did not compile"))
	(if (or (= expected actual) (and (nan? expected) (nan? actual)))
	  (do
	    (prin ".")
	    true)
	  (do
	    (print "x expected " expected " actual: " actual " args: " (string/format "%p" args))
	    (pp (disasm f))
	    false))))))

(def my-double
  (asm '{:bytecode @[(ldi 1 0x2)  # $1 = 2
		     (mul 2 0 1)  # $2 = $0 * $1
		     (ret 2)]     # return $2
	 :arity 1}))
(jit-result-matches my-double 123)

(def my-inc
  (asm '{:bytecode @[(ldi 1 0x1)  # $1 = 1
		     (add 2 0 1)  # $2 = $0 + $1
		     (ret 2)]     # return $2
	 :arity 1}))              # arg 0 is $0
(jit-result-matches my-inc 123)

(def my-fib
  # janet/test/suite-asm.janet
  (asm '{:bytecode @[(ltim 1 0 0x2)    # $1 = $0 < 2
		     (jmpif 1 :done)   # if ($1) goto :done
		     (lds 1)           # $1 = self
		     (addim 0 0 -0x1)  # $0 = $0 - 1
		     (push 0)          # push($0), for next func call
		     (call 2 1)        # $2 = call($1)
		     (addim 0 0 -0x1)  # $0 = $0 - 1
		     (push 0)          # push($0)
		     (call 0 1)        # $0 = call($1)
		     (add 0 0 2)       # $0 = $0 + $2
		     :done
		       (ret 0)]          # return $0
	 :arity 1}))
(jit-result-matches my-fib 5)

# add: $dest = $lhs + $rhs
(def my-add (asm '{:bytecode @[(add 2 0 1)  # $2 = $0 + $1
			       (ret 2)]     # return $2
		   :arity 2}))              # args 0 and 1 are $0 and $1
(jit-result-matches my-fib 5)

# addim: $dest = $lhs + im
(def my-add-im (asm '{:bytecode @[(addim 1 0 0x1)  # $1 = $0 + 1
				  (ret 1)]         # return $1
		      :arity 1}))                  # arg 0 is $0
(jit-result-matches my-add-im 5)

# band: $dest = $lhs & $rhs
(def my-band (asm '{:bytecode @[(band 0 0 1)
				(ret 0)]
		    :arity 2}))
(jit-result-matches my-band 5 5)

# bnot: $dest = ~$operand
(def my-bnot (asm '{:bytecode @[(bnot 0 0)
				(ret 0)]
		    :arity 1}))
(jit-result-matches my-bnot 5)

# bor: $dest = $lhs | $rhs
(def my-or (asm '{:bytecode @[(bor 0 0 1)
			      (ret 0)]
		  :arity 2}))
(jit-result-matches my-or 2r11110000 2r00001111)

# bxor: $dest = $lhs ^ $rhs
(def my-xor (asm '{:bytecode @[(bxor 0 0 1)
		    (ret 0)]
	:arity 2}))
(jit-result-matches my-xor 2r11110000 2r00001111)

# call: $dest = call($callee, args)
(def my-call (asm ~{:constants [,type]
	:bytecode @[(push 0)
		    (ldc 1 0)
		    (call 0 1)
		    (ret 0)]
	:arity 1}))
(jit-result-matches my-call :smile)

# Closure is not currently supported
# clo: $dest = closure(defs[$index])
# (asm ~{:defs @[,(disasm (asm '{:arity 1
#                                :bytecode @[(addim 1 0 0x8)
# 				             (ret 1)]}))]
# 	   :bytecode @[(push 0)
# 		       (clo 0 0)
# 		       (call 1 0)
# 		       (ret 1)]
# 	   :arity 1})

# cmp: $dest = janet_compare($lhs, $rhs)
(def my-cmp (asm ~{:bytecode @[(cmp 2 0 1)
			       (ret 2)]
		   :arity 2}))
(jit-result-matches my-cmp 100 200)

# cncl: resume $fiber, but raise $error immediately
# (try
#   ((asm ~{:bytecode @[(cncl 2 0 1)
# 		      (ret 2)]
# 	  :arity 2})
#    (coro 1) "Oops!")
#   ([e]
#    (string "Ah..." e)))
# # =>
# "Ah...Oops!"

# div: $dest = $lhs / $rhs
(def my-div (asm ~{:bytecode @[(div 2 0 1)
			       (ret 2)]
		   :arity 2}))
(jit-result-matches my-div 100 200)

# divim: $dest = $lhs / im
(def my-div-im (asm ~{:bytecode @[(divim 2 0 0x2)
				  (ret 2)]
		      :arity 1}))
(jit-result-matches my-div-im 100)

# eq: $dest = $lhs == $rhs
(def my-eq (asm ~{:bytecode @[(eq 2 0 1)
		    (ret 2)]
	:arity 2}))
(jit-result-matches my-eq 100 400)
(jit-result-matches my-eq 100 100)

# eqim: $dest = $lhs == im
(def my-eq-im (asm ~{:bytecode @[(eqim 2 0 0x1)
		    (ret 2)]
	:arity 1}))
(jit-result-matches my-eq-im 100)
(jit-result-matches my-eq-im 1)

# TODO: come back to this!
# err: throw $error
# (try
#   ((asm ~{:bytecode @[(err 0)]
# 	  :arity 1})
#    "at you!")
#   ([e]
#    (string "Have " e)))

# get: $dest = $ds[$key]
(def my-get (asm ~{:bytecode @[(get 2 0 1)  # (get dest ds key)
		    (ret 2)]
	:arity 2}))
(jit-result-matches my-get {:a 1} :a)

# geti: $dest = $ds[index]
(def my-geti (asm ~{:bytecode @[(geti 2 0 0x2)  # (geti dest ds index)
		    (ret 2)]
	:arity 1}))
(jit-result-matches my-geti [0 1 5])

# gt: $dest = $lhs > $rhs
(def my-gt (asm ~{:bytecode @[(gt 2 0 1)
			      (ret 2)]
		  :arity 2}))
(jit-result-matches my-gt 10 20)
(jit-result-matches my-gt 20 10)

# gte: $dest = $lhs >= $rhs
(def my-gte (asm ~{:bytecode @[(gte 2 0 1)
		    (ret 2)]
	:arity 2}))
(jit-result-matches my-gte 88 88)
(jit-result-matches my-gte 88 81)

# gtim: $dest = $lhs > im
(def my-gtim (asm ~{:bytecode @[(gtim 1 0 0x57)
		    (ret 1)]
	:arity 1}))
(jit-result-matches my-gtim 88)

# in: $dest = $ds[$key] using `in`
(def my-in (asm ~{:bytecode @[(in 2 0 1)
		       (ret 2)]
	   :arity 2}))
(jit-result-matches my-in [1 2 3] 1)
(jit-result-matches my-in {:x :spot} :x)

# jmp: pc += offset
(def my-pc-pluseq (asm ~{:bytecode @[(jmp 3)
		    (ldi 0 0x8)
		    (ret 0)
		    (ldi 0 0x9)
		    (ret 0)]
	:arity 0}))
(jit-result-matches my-pc-pluseq)

# jpmif: if $cond pc += offset else pc++
(def my-jmpif (asm ~{:bytecode @[(jmpif 0 2)
		    (ret 1)
		    (ret 2)]
	:arity 3}))
(jit-result-matches my-jmpif true :ant :bee)


# jmpni: if $cond == nil pc += offset else pc++
(def my-jmpni (asm ~{:bytecode @[(jmpni 0 2)
				  (ret 1)
				  (ret 2)]
		      :arity 3}))
(jit-result-matches my-jmpni true :ant :bee)
(jit-result-matches my-jmpni nil :ant :bee)

# jmpnn: if $cond != nil pc += offset else pc++
(def my-jmpnn (asm ~{:bytecode @[(jmpnn 0 2)
				 (ret 1)
				 (ret 2)]
		     :arity 3}))
(jit-result-matches my-jmpnn true :ant :bee)
(jit-result-matches my-jmpnn nil :ant :bee)


# jmpno: if $cond pc++ else pc += offset
(def my-jmpno (asm ~{:bytecode @[(jmpno 0 2)
		    (ret 1)
		    (ret 2)]
	:arity 3}))
(jit-result-matches my-jmpno true :ant :bee)

# ldc: $dest = constants[index]
(def my-ldc (asm ~{:constants [:grin]
	:bytecode @[(ldc 0 0)
		    (ret 0)]
	:arity 0}))
(jit-result-matches my-ldc)
# ldf: $dest = false
(def my-ldf (asm ~{:bytecode @[(ldf 0)
		    (ret 0)]
	:arity 0}))
(jit-result-matches my-ldf)

# ldi: $dest = integer
(def my-ldi (asm ~{:bytecode @[(ldi 0 0x100)
		    (ret 0)]
	:arity 0}))
(jit-result-matches my-ldi)

# ldn: $dest = nil
(def my-ldn (asm ~{:bytecode @[(ldn 0)
		    (ret 0)]
	:arity 0}))
(jit-result-matches my-ldn)

   # factorial with accumulator
   # lds: $dest = current closure (self)
(def my-lds (asm ~{:bytecode @[(ltim 2 0 0x2)
			       (jmpif 2 :end)
			       (lds 2)
			       (mul 1 0 1)
			       (addim 0 0 -0x1)
			       (push2 0 1)
			       (call 1 2)
			       :end
				 (ret 1)]
		   :arity 2}))
(jit-result-matches my-lds 5 1)

   # ldt: $dest = true
(def my-ldt (asm ~{:bytecode @[(ldt 0)
			       (ret 0)]
		   :arity 0}))
(jit-result-matches my-ldt)

# ldu not supported
# ldu: $dest = envs[env][index]
# (def my-ldu (asm '{:arity 1
# 		   :bytecode @[(clo 1 0)             # $1 = defs[0]
# 			       (ldi 2 0x8)           # $2 = 8
# 			       (push 2)              # push $2 to args
# 			       (call 2 1)            # $2 = ($1 $2) == (defs[0] $2)
# 			       (ret 2)]              # return $2
# 		   :defs @[{:arity 1
# 			    :bytecode @[(ldu 1 0 0)  # $1 = $0 from parent?
# 					(add 2 1 0)  # $2 = $1 + $0
# 					(ret 2)]     # return $2
# 			    :environments @[-1]}]})) # (ldu _ 0 _) refers to -1
# (jit-result-matches my-ldu 3)

# len: $dest = length($ds)
(def my-len (asm ~{:bytecode @[(len 0 0)
			       (ret 0)]
		   :arity 1}))
(jit-result-matches my-len [:ant :bee :cat])

# lt: $dest = $lhs < $rhs
(def my-lt (asm ~{:bytecode @[(lt 0 0 1)
			      (ret 0)]
		  :arity 2}))
(jit-result-matches my-lt math/-inf math/inf)

# lte: $dest = $lhs <= $rhs
(def my-lte (asm ~{:bytecode @[(lte 0 0 1)
			       (ret 0)]
		   :arity 2}))
(jit-result-matches my-lt 0 1)

# ltim: $dest = $lhs < im
(def my-ltim (asm ~{:bytecode @[(ltim 0 0 0x1)
				(ret 0)]
		    :arity 1}))
(jit-result-matches my-ltim 0)

# mkarr: $dest = call(array, args)
(def my-mkarr (asm ~{:bytecode @[(push3 0 1 2)
				 (mkarr 0)
				 (ret 0)]
		     :arity 3}))
(jit-result-deep-matches my-mkarr :elephant :fox :giraffe)

# mkbtp: $dest = call(tuple/brackets, args)
(def my-mkbtp (asm ~{:bytecode @[(push2 0 1)
				 (mkbtp 0)
				 (ret 0)]
		     :arity 2}))
(jit-result-matches my-mkbtp [] {})

# mkbuf: $dest = call(buffer, args)
(def my-mkbuf (asm ~{:bytecode @[(push2 0 1)
				 (mkbuf 0)
				 (ret 0)]
		     :arity 2}))
(jit-result-deep-matches my-mkbuf :hi " there")

# mkstr: $dest = call(string, args)
(def my-mkstr (asm ~{:bytecode @[(push2 0 1)
				 (mkstr 0)
				 (ret 0)]
		     :arity 2}))
(jit-result-matches my-mkstr @"gday, m" 8)

# mkstu: $dest = call(struct, args)
(def my-mkstu (asm ~{:bytecode @[(push3 0 1 2)
				 (push3 3 4 5)
				 (mkstu 0)
				 (ret 0)]
		     :arity 6}))

(jit-result-matches my-mkstu :x 10 :y 20 :z 80)

# mktab: $dest = call(table, args)
(def my-mktab (asm ~{:bytecode @[(push2 0 1)
				 (mktab 0)
				 (ret 0)]
		     :arity 2}))
(jit-result-deep-matches my-mktab :breathe :slowly)

# mktup: $dest = call(tuple, args)
(def my-mktup (asm ~{:bytecode @[(push3 0 1 2)
				 (mktup 0)
				 (ret 0)]
		     :arity 3}))
(jit-result-matches my-mktup '+ 1 1)

# mod: $dest = $lhs mod $rhs
(def my-mod (asm ~{:bytecode @[(mod 0 0 1)
		    (ret 0)]
	:arity 2}))
(jit-result-matches my-mod -3 2)

# movf: $dest = $src
(def my-movf (asm ~{:bytecode @[(movf 0 1)  # (movf src dest)
				(ret 1)]
		    :arity 1}))
(jit-result-matches my-movf :echo)

# movn: $dest = $src
(def my-movn (asm ~{:bytecode @[(movn 1 0)  # (movn dest src)
				(ret 1)]
		    :arity 1}))
(jit-result-matches my-movn :again)

# mul: $dest = $lhs * $rhs
(def my-mul (asm ~{:bytecode @[(mul 2 0 1)
		    (ret 2)]
	:arity 2}))
(jit-result-matches my-mul 2 3)

# mulim: $dest = $lhs * im
(def my-mulim (asm ~{:bytecode @[(mulim 1 0 0x8)
		    (ret 1)]
	:arity 1}))
(jit-result-matches my-mulim 11)

# neq: dest = $lhs != $rhs
(def my-neq (asm ~{:bytecode @[(neq 2 0 1)
			       (ret 2)]
		   :arity 2}))
(jit-result-matches my-neq 0 -0)

# neqim: $dest = $lhs != im
(def my-neqim (asm ~{:bytecode @[(neqim 2 0 0x23)
				 (ret 2)]
		     :arity 1}))
(jit-result-matches my-neqim 22)

# next: $dest = next($ds, $key)
(def my-next (asm ~{:bytecode @[(next 2 0 1)
				(ret 2)]
		    :arity 2}))
(jit-result-matches my-next [:a :b :c] 1)

# noop: does nothing
(def my-noop (asm ~{:bytecode @[(noop)
		    (noop)
		    (noop)
		    (noop)
		    (noop)
		    (noop)
		    (noop)
		    (noop)
		    (ret 0)]
	:arity 1}))
(jit-result-matches my-noop math/inf)

# Propogate is not supported
# prop: propagate (re-raise) a signal that has been caught
# (do
#   (def fib (coro :a))
#   (resume fib)
#   ((asm ~{:bytecode @[(prop 2 0 1)  # (prop dest val fiber)
# 		      (ldi 0 0x9)   # never reached
# 		      (ret 0)]
# 	  :arity 2})
#    "skip!" fib))

# put: $ds[$key] = $val
(def my-put (asm ~{:bytecode @[(put 0 1 2)  # (put ds key val)
			       (ret 0)]
		   :arity 3}))
(jit-result-matches my-put @{} :a 1)

# puti: $ds[index] = $val
(def my-puti (asm ~{:bytecode @[(puti 0 1 0x0)  # (puti ds val index)
				(ret 0)]
		    :arity 2}))
(jit-result-matches my-puti @[] :smile)

# rem: $dest = $lhs % $rhs
(def my-rem (asm ~{:bytecode @[(rem 2 0 1)
			       (ret 2)]
		   :arity 2}))
(jit-result-matches my-rem -3 2)

# resume is not supported
# res: $dest = resume $fiber with $val
# ((asm ~{:bytecode @[(res 2 0 1)
# 		       (ret 2)]
# 	   :arity 2})
#  (fiber/new (fn [x] (* x 8)))
#  9)

# ret: return $val
(def my-ret (asm ~{:bytecode @[(ret 0)]
		   :arity 1}))
(jit-result-matches my-ret :love)

# retn: return nil
(def my-retn (asm ~{:bytecode @[(retn)]
		    :arity 0}))
(jit-result-matches my-retn)

# setu: envs[env][index] = $val
# ((asm '{:arity 0
# 	:bytecode @[(clo 0 0)                 # define the closure
# 		    (ldi 1 0x8)               # prepare $1 for closure
# 		    (call 2 0)                # calling for side-effect
# 		    (ret 1)]                  # returned modified value
# 	:defs @[{:arity 0
# 		 :bytecode @[(ldu 0 0 1)      # $0 = parent's $1
# 			     (addim 0 0 0x1)  # increment $0
# 			     (setu 0 0 1)     # parent's $1 = $0
# 			     (retn)]          # caller ignores this
# 		 :environments @[-1]}]}))

# enum JanetSignal, JANET_SIGNAL_ERROR is 1
# sig: $dest = emit $value as sigtype
# (try
#   ((asm ~{:bytecode @[(sig 2 0 0x1)  # (sig dest value sigtype)
# 		      (ldi 1 0xff)   # never reached
# 		      (ret 1)]
# 	  :arity 1})
#    :wocky)
#   ([e]
#    (string "jabber" e)))

# sl: $dest = $lhs << $rhs
(def my-sl (asm ~{:bytecode @[(sl 2 0 1)
			      (ret 2)]
		  :arity 2}))
(jit-result-matches my-sl 2r10 3)

# slim: $dest = $lhs << shamt
(def my-slim (asm ~{:bytecode @[(slim 1 0 0x3)
			      (ret 1)]
		  :arity 1}))
(jit-result-matches my-slim 2r10)

# sr: $dest = $lhs >> $rhs
(def my-sr (asm ~{:bytecode @[(sr 2 0 1)
			      (ret 2)]
		  :arity 2}))
(jit-result-matches my-sr -2r101 2)

# srim: $dest = $lhs >> shamt
(def my-srim (asm ~{:bytecode @[(srim 1 0 0x2)
			      (ret 1)]
		  :arity 1}))
(jit-result-matches my-srim -2r101)

# sru: $dest = $lhs >>> $rhs
(def my-sru (asm ~{:bytecode @[(sru 2 0 1)
			      (ret 2)]
		  :arity 2}))
(jit-result-matches my-sru 2r1100 3)

# sruim: $dest = $lhs >>> shamt
(def my-sruim (asm ~{:bytecode @[(sruim 1 0 0x3)
			      (ret 1)]
		  :arity 1}))
(jit-result-matches my-sruim 2r1100)

# sub: $dest = $lhs - $rhs
(def my-sub (asm ~{:bytecode @[(sub 2 0 1)
			      (ret 2)]
		  :arity 2}))
(jit-result-matches my-sub 0 1)

# tcall: return call($callee, args)
(def my-tcall (asm ~{:bytecode @[(tcall 0)]
		     :arity 1}))
(jit-result-matches my-tcall +)

# enum JanetType, JANET_KEYWORD is 6, 2 ** 6 == 64
# tchck: assert $slot matches types
(def my-tcheck (asm ~{:bytecode @[(tchck 0 64)
				  (ret 0)]
		      :arity 1}))
(jit-result-matches my-tcheck :hello)
(print "DONE")
