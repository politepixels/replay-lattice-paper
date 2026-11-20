(*
  Copyright 2026 PolitePixels Limited

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*)

From Stdlib Require Import List Arith Lia Floats.
From iris.program_logic Require Import lifting adequacy.
From iris.proofmode Require Import proofmode.
Require Import ReplayLattice ReplayLatticeEval.
Import ListNotations.

(**
  [add_from] adds binary64 operands from left to right, beginning with the supplied
  accumulator.
*)
Definition add_from := chain float PrimFloat.add.

(**
  [block] represents one ordered group of binary64 operands assigned to a worker.
*)
Definition block := list float.

(**
  [block_sum] computes a single block's serial binary64 sum from zero.
*)
Definition block_sum := add_from 0%float.

(**
  [assign] divides consecutive whole blocks among workers in input order. Changing
  the worker count preserves the block boundaries.

  Takes [extra] ([nat]) which is the number of workers in addition to the first.
  Takes [bs] ([list block]) which holds the blocks to distribute.
*)
Fixpoint assign (extra : nat) (bs : list block) : list (list block) :=
  match extra with
  | O => [bs]
  | S n =>
      let k := Nat.div (length bs + S n) (S (S n)) in
      firstn k bs :: assign n (skipn k bs)
  end.

(**
  [slots] maps worker indices to optional lists of published block sums. An
  assigned worker's slot is empty until it publishes.
*)
Definition slots := nat -> option (list float).

(**
  [empty_slots] is the shared state at launch, before any worker has published its
  block sums.
*)
Definition empty_slots : slots := fun _ => None.

(**
  [publish] fills one worker's slot and preserves every other slot.

  Takes [m] ([slots]) which is the current slot map.
  Takes [i] ([nat]) which identifies the publishing worker.
  Takes [xs] ([list float]) which holds its completed block sums in input order.
*)
Definition publish (m : slots) (i : nat) (xs : list float) : slots :=
  fun j => if Nat.eq_dec j i then Some xs else m j.

(**
  [thread] represents a worker or the joining thread. A worker keeps its running sum
  private and publishes its completed block sums together.
*)
Inductive thread :=
  (**
    [Launch] prepares one worker per block group and starts the join at slot zero.

    Takes [groups] ([list (list block)]) which holds one ordered group of blocks per
    worker.
  *)
  | Launch (groups : list (list block))

  (**
    [Work] selects the worker's next block or publishes its completed sums when its
    queue is empty.

    Takes [slot] ([nat]) which identifies this worker's publication slot.
    Takes [todo] ([list block]) which holds the remaining whole blocks.
    Takes [finished] ([list float]) which holds completed block sums in reverse
    order.
  *)
  | Work (slot : nat) (todo : list block) (finished : list float)

  (**
    [SumBlock] adds the remaining elements of the current block before returning to
    the worker's queue.

    Takes [slot] ([nat]) which identifies this worker's publication slot.
    Takes [xs] ([block]) which holds the unconsumed operands of the current block.
    Takes [acc] ([float]) which is its running sum.
    Takes [todo] ([list block]) which holds the blocks still to start.
    Takes [finished] ([list float]) which holds completed block sums in reverse
    order.
  *)
  | SumBlock (slot : nat) (xs : block) (acc : float)
             (todo : list block) (finished : list float)

  (**
    [Join] polls the next worker slot, returning the accumulator when all slots have
    been consumed.

    Takes [first] ([nat]) which is the next slot to read.
    Takes [count] ([nat]) which is the number of slots left to read.
    Takes [acc] ([float]) which is the sum accumulated so far.
  *)
  | Join (first count : nat) (acc : float)

  (**
    [Merge] adds the published block sums from one slot before polling the next.

    Takes [xs] ([list float]) which holds the published sums still to add.
    Takes [acc] ([float]) which is the running total.
    Takes [first] ([nat]) which is the next slot to poll.
    Takes [count] ([nat]) which is the number of later slots still to read.
  *)
  | Merge (xs : list float) (acc : float) (first count : nat)

  (**
    [Return] is the terminal thread state.

    Takes [v] ([float]) which is the thread's return value.
  *)
  | Return (v : float).

(**
  [workers_from] creates one worker per group and gives successive workers
  consecutive publication slots.

  Takes [i] ([nat]) which is the first worker slot in the group.
  Takes [gs] ([list (list block)]) which holds each worker's blocks in input order.
*)
Fixpoint workers_from (i : nat) (gs : list (list block)) : list thread :=
  match gs with
  | [] => []
  | g :: rest => Work i g [] :: workers_from (S i) rest
  end.

(**
  [advance] performs one thread step and at most one binary64 addition. The joining
  thread polls worker slots in index order. A successful step returns the next thread
  state, the updated slots and any new workers.

  Takes [t] ([thread]) which is the thread selected by the scheduler.
  Takes [m] ([slots]) which is the current state of the worker slots.
*)
Definition advance (t : thread) (m : slots) : option (thread * slots * list thread) :=
  match t with
  | Launch gs => Some (Join 0 (length gs) 0%float, m, workers_from 0 gs)
  | Work i [] done => Some (Return 0%float, publish m i (rev done), [])
  | Work i (b :: bs) done => Some (SumBlock i b 0%float bs done, m, [])
  | SumBlock i [] a bs done => Some (Work i bs (a :: done), m, [])
  | SumBlock i (x :: xs) a bs done =>
      Some (SumBlock i xs (PrimFloat.add a x) bs done, m, [])
  | Join _ O a => Some (Return a, m, [])
  | Join i (S n) a =>
      match m i with
      | None => Some (t, m, [])
      | Some xs => Some (Merge xs a (S i) n, m, [])
      end
  | Merge [] a i n => Some (Join i n a, m, [])
  | Merge (x :: xs) a i n => Some (Merge xs (PrimFloat.add a x) i n, m, [])
  | Return _ => None
  end.

(**
  [result] extracts the value of a returned thread. Every other thread state has an
  execution step available.

  Takes [t] ([thread]) which is the current thread state.
*)
Definition result (t : thread) : option float :=
  match t with Return v => Some v | _ => None end.

(**
  [thread_step] relates the states before and after one successful call to
  [advance]. This language emits an empty observation list at each step.

  Takes [t] ([thread]) which is the current thread state.
  Takes [m] ([slots]) which is the slot map on entry.
  Takes [obs] ([list unit]) which holds the observations of the step.
  Takes [t'] ([thread]) which is the successor thread.
  Takes [m'] ([slots]) which is the successor slot map.
  Takes [forks] ([list thread]) which holds the children created by this step.
*)
Definition thread_step t m (obs : list unit) t' m' forks :=
  obs = [] /\ advance t m = Some (t', m', forks).

(**
  [thread_mixin] proves the Iris language laws for [Return], [result] and
  [thread_step], including that a returned value cannot take a step.
*)
Lemma thread_mixin : LanguageMixin Return result thread_step.
Proof.
  constructor.
  - reflexivity.
  - intros [] value; simpl; congruence.
  - intros [] m k t' m' forks [_ H]; try reflexivity.
    discriminate.
Qed.

(**
  [replay_language] packages the thread semantics as an Iris language for the
  adequacy proof.
*)
Canonical Structure replay_language := Language thread_mixin.

(**
  [slots_correct] requires every published slot to contain its expected block sums.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [m] ([slots]) which is the current state of the worker slots.
*)
Definition slots_correct (expected m : slots) :=
  forall i xs, m i = Some xs -> expected i = Some xs.

(**
  [assigned] requires each worker's expected slot to contain the sums of its assigned
  blocks in order.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [i] ([nat]) which is the index of the first worker in the group.
  Takes [gs] ([list (list block)]) which holds the block group for each worker.
*)
Fixpoint assigned (expected : slots) (i : nat) (gs : list (list block)) : Prop :=
  match gs with
  | [] => True
  | g :: rest => expected i = Some (map block_sum g) /\ assigned expected (S i) rest
  end.

(**
  [joins_correct] requires that merging the remaining expected slots in order
  produces a value satisfying the postcondition.

  Takes [expected] ([slots]) which holds the block sums expected from each worker.
  Takes [Q] ([float -> Prop]) which is the postcondition for the final sum.
  Takes [next_slot] ([nat]) which is the next slot to merge.
  Takes [remaining] ([nat]) which is the number of slots still to merge.
  Takes [acc] ([float]) which is the accumulator after the preceding slots.
*)
Fixpoint joins_correct (expected : slots) (Q : float -> Prop) next_slot remaining acc : Prop :=
  match remaining with
  | O => Q acc
  | S k => exists xs, expected next_slot = Some xs /\
                      joins_correct expected Q (S next_slot) k (add_from acc xs)
  end.

(**
  [correct] relates a thread's current state to its remaining work. A worker's
  sums must match its expected slot. For a join, merging the remaining expected
  slots in order must satisfy the postcondition.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [Q] ([float -> Prop]) which is the postcondition for this thread's return
  value.
  Takes [t] ([thread]) which is the current thread state.
*)
Definition correct (expected : slots) (Q : float -> Prop) (t : thread) : Prop :=
  match t with
  | Launch gs => assigned expected 0 gs /\ Q (add_from 0%float (map block_sum (concat gs)))
  | Work i bs done => Q 0%float /\ expected i = Some (rev done ++ map block_sum bs)
  | SumBlock i xs a bs done =>
      Q 0%float /\ expected i = Some (rev done ++ add_from a xs :: map block_sum bs)
  | Join i n a => joins_correct expected Q i n a
  | Merge xs a i n => joins_correct expected Q i n (add_from a xs)
  | Return v => Q v
  end.

(**
  [add_from_app] proves that a serial sum can pause between two input segments and
  resume from its accumulator without changing the order of additions.

  Takes [a] ([float]) which is the incoming accumulator.
  Takes [xs] and [ys] ([list float]) which are the first and second segments
  respectively.
*)
Lemma add_from_app a xs ys :
  add_from a (xs ++ ys) = add_from (add_from a xs) ys.
Proof.
  revert a.
  induction xs as [| x xs IH]; intros a; simpl; auto.
Qed.

(**
  [joins_assigned] establishes the join invariant from the assigned groups and a
  postcondition on their ordered block sum.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [Q] ([float -> Prop]) which is the postcondition for the returned value.
  Takes [gs] ([list (list block)]) which holds each worker's blocks in input order.
  Takes [i] ([nat]) which is the first worker slot in the group.
  Takes [a] ([float]) which is the incoming accumulator.
*)
Lemma joins_assigned expected (Q : float -> Prop) gs i a :
  assigned expected i gs -> Q (add_from a (map block_sum (concat gs))) ->
  joins_correct expected Q i (length gs) a.
Proof.
  revert i a.
  induction gs as [| g gs IH]; intros i a H Hresult; simpl in *.
  - exact Hresult.
  - destruct H as [Hslot Hrest].
    exists (map block_sum g).
    split.
    + exact Hslot.
    + apply IH.
      { exact Hrest. }
      by rewrite map_app add_from_app in Hresult.
Qed.

(**
  [workers_assigned] proves that each newly created worker satisfies its thread
  invariant when its blocks have been assigned to the expected slot.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [gs] ([list (list block)]) which holds each worker's blocks in input order.
  Takes [i] ([nat]) which is the first worker slot in the group.
*)
Lemma workers_assigned expected gs i :
  assigned expected i gs ->
  Forall (correct expected (fun _ => True)) (workers_from i gs).
Proof.
  revert i.
  induction gs as [| g gs IH]; intros i H; simpl in *; constructor.
  - split.
    + exact I.
    + exact (proj1 H).
  - apply IH.
    exact (proj2 H).
Qed.

(**
  [advance_correct] shows that a step preserves the slot invariant and the running
  thread's obligation, and establishes the obligations of any newly forked workers.

  Takes [expected] ([slots]) which is the slot map fixed at launch.
  Takes [Q] ([float -> Prop]) which is the postcondition for the returned value.
  Takes [t] ([thread]) which is the current thread state.
  Takes [m] ([slots]) which is the slot map on entry.
  Takes [t'] ([thread]) which is the successor thread.
  Takes [m'] ([slots]) which is the successor slot map.
  Takes [forks] ([list thread]) which holds the children created by the step.
*)
Lemma advance_correct expected Q t m t' m' forks :
  correct expected Q t -> slots_correct expected m ->
  advance t m = Some (t', m', forks) ->
  slots_correct expected m' /\ correct expected Q t' /\
  Forall (correct expected (fun _ => True)) forks.
Proof.
  destruct t as
    [ gs
    | i [| b bs] done
    | i [| x xs] a bs done
    | i [| n] a
    | [| x xs] a i n
    | v
    ]; intros H Hm Hstep; simpl in H, Hstep.
  all: try (injection Hstep as <- <- <-).
  all: try discriminate.
  all: try solve [split; first exact Hm; split; [exact H | constructor]].

  - destruct H as [Hgs HQ].
    split.
    { exact Hm. }
    split.
    + apply joins_assigned; done.
    + apply workers_assigned; done.

  - destruct H as [HQ H].
    split.
    + intros j ys.
      unfold publish.
      destruct (Nat.eq_dec j i) as [-> | Hne].
      * intros [= <-].
        by rewrite app_nil_r in H.
      * apply Hm.
    + split.
      * exact HQ.
      * constructor.

  - split.
    { exact Hm. }
    split.
    + simpl.
      by rewrite <- app_assoc.
    + constructor.

  - destruct (m i) as [vs |] eqn:Hi; injection Hstep as <- <- <-.
    all: split; [exact Hm |].
    + split.
      * destruct H as (ys & Hy & HQ).
        specialize (Hm _ _ Hi).
        by rewrite Hm in Hy; injection Hy as <-.
      * constructor.
    + split.
      * exact H.
      * constructor.
Qed.

(**
  [advance_exists] proves that every thread which has yet to return can take a step,
  including a join polling an empty slot.

  Takes [t] ([thread]) which is the current thread state.
  Takes [m] ([slots]) which is the slot map on entry.
*)
Lemma advance_exists t m :
  result t = None -> exists t' m' forks, advance t m = Some (t', m', forks).
Proof.
  destruct t as
    [ gs
    | i [| b bs] done
    | i [| x xs] a bs done
    | i [| n] a
    | [| x xs] a i n
    | v
    ]; simpl; try discriminate; intros _.
  all: try (do 3 eexists; reflexivity).
  destruct (m i); do 3 eexists; reflexivity.
Qed.

Section weakest_precondition.

  (**
    [Σ] supplies the Iris ghost-state signature. [Hinv] provides its invariant
    resources and [expected] fixes the worker slots used by the state
    interpretation.
  *)
  Context {Σ : gFunctors} {Hinv : invGS Σ} (expected : slots).

  (**
    [replay_iris] interprets shared memory using [slots_correct]. Worker publication
    preserves the state interpretation used by the weakest-precondition proof.
  *)
  Local Instance replay_iris : irisGS replay_language Σ :=
    IrisG Hinv (fun m _ _ _ => ⌜slots_correct expected m⌝%I)
               (fun _ => True%I) (fun _ => 0) (fun _ _ _ _ => fupd_intro _ _).

  (**
    [thread_wp] lifts preservation of the slot invariant from one step to an Iris
    weakest precondition. It covers the running thread and every worker it forks.

    Takes [t] ([thread]) which is the thread whose execution is verified.
    Takes [Q] ([float -> Prop]) which is the postcondition for its return value.
  *)
  Lemma thread_wp t Q :
    correct expected Q t -> ⊢ WP t {{ v, ⌜Q v⌝ }}.
  Proof.
    intros Ht.
    iLöb as "IH" forall (t Q Ht).
    destruct (result t) as [v |] eqn:Hv.
    - apply (mixin_of_to_val _ _ _ thread_mixin) in Hv.
      subst t.
      iApply wp_value'.
      by iPureIntro.
    - iApply wp_lift_step.
      { exact Hv. }
      iIntros (m ns k ks nt) "%Hm".
      iApply fupd_mask_intro.
      { set_solver. }
      iIntros "Hclose".

      iSplit.
      { iPureIntro.
        destruct (advance_exists t m Hv) as (next & m' & forks & Hstep).
        exists [], next, m', forks.
        done. }

      iNext.
      iIntros (next m' forks) "%Hstep _".
      destruct Hstep as [_ Hstep].
      iMod "Hclose" as "_".
      destruct (advance_correct expected Q t m next m' forks Ht Hm Hstep)
        as (Hm' & Hnext & Hforks).

      iModIntro.
      iSplit.
      { by iPureIntro. }
      iSplitR.
      + by iApply "IH".
      + iApply big_sepL_intro.
        iIntros "!>" (j child Hj).
        assert (Hchild : correct expected (fun _ => True) child).
        { eapply Forall_lookup_1; eauto. }
        iApply (wp_wand with "(IH [%])").
        { exact Hchild. }
        by iIntros (?) "_".
  Qed.
End weakest_precondition.

(**
  [assigned_lookup] turns consecutive slot lookups into the assignment invariant for
  a list of worker groups.

  Takes [gs] ([list (list block)]) which holds each worker's blocks in input order.
  Takes [i] ([nat]) which is the first worker slot in the group.
  Takes [expected] ([slots]) which is the slot map fixed at launch.
*)
Lemma assigned_lookup gs i expected :
  (forall j, expected (i + j) = nth_error (map (map block_sum) gs) j) ->
  assigned expected i gs.
Proof.
  revert i.
  induction gs as [| g gs IH]; intros i H; simpl.
  - done.
  - split.
    + specialize (H O).
      by rewrite Nat.add_0_r in H.
    + apply IH.
      intros j.
      specialize (H (S j)).
      by rewrite Nat.add_succ_r in H.
Qed.

(**
  [assign_concat] proves that concatenating the assigned worker groups recovers all
  original blocks in their original order.

  Takes [extra] ([nat]) which is the number of workers beyond the first.
  Takes [bs] ([list block]) which holds the blocks to distribute.
*)
Lemma assign_concat extra bs : concat (assign extra bs) = bs.
Proof.
  revert bs.
  induction extra as [| n IH]; intros bs.
  - simpl.
    apply app_nil_r.
  - cbn [assign concat].
    rewrite IH.
    apply firstn_skipn.
Qed.

(**
  [replay_sum] launches a join and assigns fixed-width blocks to the requested
  workers. A request for zero workers launches an empty join.

  Takes [workers] ([nat]) which is the requested worker count.
  Takes [xs] ([list float]) which holds the input values in order.
*)
Definition replay_sum (workers : nat) (xs : list float) :=
  Launch (match workers with
          | O => []
          | S extra => assign extra (chunks float (length xs) EVAL_BLOCK xs)
          end).

(**
  [replay_sum_forks_workers] proves that the launch step creates exactly one child
  per requested worker and starts the join at slot zero.

  Takes [workers] ([nat]) which is the requested worker count.
  Takes [xs] ([list float]) which holds the input values in order.
  Takes [m] ([slots]) which is the slot map before the launch step.
*)
Lemma replay_sum_forks_workers workers xs m :
  exists children, advance (replay_sum workers xs) m =
    Some (Join 0 workers 0%float, m, children) /\ length children = workers.
Proof.
  assert (Hassign : forall extra bs, length (assign extra bs) = S extra).
  { induction extra; intros bs; simpl; auto. }
  assert (Hfork : forall gs i, length (workers_from i gs) = length gs).
  { induction gs; intros i; simpl; auto. }

  destruct workers as [| extra]; cbn [replay_sum advance]; eexists.
  - split; reflexivity.
  - rewrite Hassign.
    split.
    + reflexivity.
    + by rewrite Hfork Hassign.
Qed.

(**
  [concurrent_replay_sum] proves that with at least one worker, no thread gets stuck
  and whenever the joining thread returns, its value is the fixed-tree sum.

  Takes [workers] ([nat]) which is the number of workers launched.
  Takes [xs] ([list float]) which holds the input values in order.
  Takes [s] ([msched]) which is the schedule supplied to the model's fixed-tree
  kernel.
*)
Theorem concurrent_replay_sum workers xs s :
  0 < workers ->
  adequate NotStuck (replay_sum workers xs) empty_slots
    (fun v _ => v = kernel_fixed_tree float PrimFloat.add 0%float s xs).
Proof.
  destruct workers as [| extra].
  { lia. }
  intros _.
  unfold replay_sum.
  set (gs := assign _ _).
  set (expected := fun i => nth_error (map (map block_sum) gs) i).

  apply (wp_adequacy invΣ replay_language).
  iIntros (Hinv ks).
  iModIntro.
  iExists (fun m _ => ⌜slots_correct expected m⌝%I), (fun _ => True%I).
  iSplit.
  { iPureIntro.
    intros i ys H.
    discriminate. }

  iApply (thread_wp expected).
  split.
  - apply assigned_lookup.
    reflexivity.
  - unfold gs.
    by rewrite assign_concat.
Qed.

(**
  [concurrent_replay_combine64] identifies the concurrent result with the binary64
  evaluator's [Replay] result on a gated schedule with at least one worker.

  Takes [s] ([msched]) which supplies the gated mode and worker count.
  Takes [xs] ([list float]) which holds the common input to the concurrent run and
  the evaluator.
*)
Corollary concurrent_replay_combine64 s xs :
  mgated s -> 0 < sworkers s ->
  adequate NotStuck (replay_sum (sworkers s) xs) empty_slots
    (fun v _ => v = combine64 Replay s xs).
Proof.
  intros Hg Hw.
  unfold mgated in Hg.
  apply Nat.eqb_neq in Hg.
  unfold combine64.
  rewrite Hg.
  by apply concurrent_replay_sum.
Qed.

(**
  [solo_step] describes a single thread transition that forks no children.

  Takes [a] and [b] ([thread * slots]) which are the configurations before and after
  the step.
*)
Definition solo_step (a b : thread * slots) :=
  advance (fst a) (snd a) = Some (fst b, snd b, []).

(**
  [schedule_solo] lifts a sequence of solo steps to an execution of the whole pool
  by scheduling just the selected thread.

  Takes [pre] and [post] ([list thread]) which are the threads before and after the
  selected thread in the pool.
  Takes [t] ([thread]) which is the current thread state.
  Takes [m] ([slots]) which is the slot map on entry.
  Takes [t'] ([thread]) which is the final state of the selected thread.
  Takes [m'] ([slots]) which is the final slot map.
*)
Lemma schedule_solo pre post t m t' m' :
  rtc solo_step (t, m) (t', m') ->
  rtc (@erased_step replay_language) (pre ++ t :: post, m) (pre ++ t' :: post, m').
Proof.
  apply (rtc_congruence (fun p : thread * slots => (pre ++ fst p :: post, snd p))).
  intros [e h] [e' h'] H.
  exists [].
  eapply step_atomic with (t1 := pre) (t2 := post) (efs := []); simpl.
  - reflexivity.
  - by rewrite app_nil_r.
  - split.
    + done.
    + exact H.
Qed.

(**
  [finish_block] constructs an execution that consumes the remaining operands of one
  block and records its sum in the worker's private results.

  Takes [i] ([nat]) which identifies the worker's publication slot.
  Takes [xs] ([block]) which holds the remaining elements of the active block.
  Takes [a] ([float]) which is the incoming accumulator.
  Takes [bs] ([list block]) which holds the worker's remaining blocks in order.
  Takes [done] ([list float]) which holds already completed block sums in reverse
  order.
  Takes [m] ([slots]) which is the slot map on entry.
*)
Lemma finish_block i xs a bs done m :
  rtc solo_step (SumBlock i xs a bs done, m) (Work i bs (add_from a xs :: done), m).
Proof.
  revert a.
  induction xs as [| x xs IH]; intros a.
  - apply rtc_once.
    reflexivity.
  - eapply rtc_l with (y := (SumBlock i xs (PrimFloat.add a x) bs done, m)).
    + reflexivity.
    + apply (IH (PrimFloat.add a x)).
Qed.

(**
  [finish_worker] constructs an execution in which one worker finishes its remaining
  blocks, publishes in input order and returns.

  Takes [i] ([nat]) which identifies the worker's publication slot.
  Takes [bs] ([list block]) which holds the worker's remaining blocks in order.
  Takes [done] ([list float]) which holds already completed block sums in reverse
  order.
  Takes [m] ([slots]) which is the slot map on entry.
*)
Lemma finish_worker i bs done m :
  rtc solo_step (Work i bs done, m)
    (Return 0%float, publish m i (rev done ++ map block_sum bs)).
Proof.
  revert done.
  induction bs as [| b bs IH]; intros done.
  - simpl.
    rewrite app_nil_r.
    apply rtc_once.
    reflexivity.
  - eapply rtc_l with (y := (SumBlock i b 0%float bs done, m)).
    { reflexivity. }
    eapply rtc_trans.
    { apply finish_block. }
    specialize (IH (block_sum b :: done)).
    simpl in IH.
    by rewrite <- app_assoc in IH.
Qed.

(**
  [finish_merge] constructs the solo steps that merge one slot's published sums and
  resume the join.

  Takes [xs] ([list float]) which holds the remaining sums from this slot.
  Takes [a] ([float]) which is the incoming accumulator.
  Takes [i] ([nat]) which is the next slot to poll.
  Takes [n] ([nat]) which is the number of slots still to poll.
  Takes [m] ([slots]) which is the slot map on entry.
*)
Lemma finish_merge xs a i n m :
  rtc solo_step (Merge xs a i n, m) (Join i n (add_from a xs), m).
Proof.
  revert a.
  induction xs as [| x xs IH]; intros a.
  - apply rtc_once.
    reflexivity.
  - eapply rtc_l with (y := (Merge xs (PrimFloat.add a x) i n, m)).
    + reflexivity.
    + apply (IH (PrimFloat.add a x)).
Qed.

(**
  [finish_groups] constructs a pool execution that finishes each worker before the
  join reads its slot and adds its block sums.

  Takes [gs] ([list (list block)]) which holds each worker's blocks in input order.
  Takes [i] ([nat]) which is the first worker slot in the group.
  Takes [a] ([float]) which is the incoming accumulator.
  Takes [done] ([list thread]) which holds the threads already in the pool between
  the join and the new workers.
  Takes [m] ([slots]) which is the slot map on entry.
*)
Lemma finish_groups gs i a done m :
  exists tail m', rtc (@erased_step replay_language)
    (Join i (length gs) a :: done ++ workers_from i gs, m)
    (Return (add_from a (map block_sum (concat gs))) :: tail, m').
Proof.
  revert i a done m.
  induction gs as [| g gs IH]; intros i a done m.
  - exists (done ++ []), m.
    apply (schedule_solo [] (done ++ [])).
    apply rtc_once.
    reflexivity.

  - set (sums := map block_sum g).
    set (m1 := publish m i sums).
    set (rest := workers_from (S i) gs).
    specialize (IH (S i) (add_from a sums) (done ++ [Return 0%float]) m1).
    destruct IH as (tail & final & IH).
    exists tail, final.
    cbn [concat length workers_from].
    rewrite map_app add_from_app.

    eapply rtc_trans with
      (y := (Join i (S (length gs)) a :: done ++ Return 0%float :: rest, m1)).
    { apply (schedule_solo (Join i (S (length gs)) a :: done) rest).
      apply (finish_worker i g [] m). }

    rewrite <- app_assoc in IH.
    simpl in IH.
    eapply rtc_trans.
    2: exact IH.
    apply (schedule_solo [] (done ++ Return 0%float :: rest)).
    eapply rtc_l.
    2: apply (finish_merge sums a (S i) (length gs) m1).
    unfold solo_step.
    simpl.
    unfold m1, publish.
    by destruct (Nat.eq_dec i i).
Qed.

(**
  [concurrent_replay_can_return] exhibits an interleaving in which the joining
  thread returns the fixed-tree sum for each input and positive worker count.
  The exhibited schedule finishes each worker before the join reads its slot.

  Takes [workers] ([nat]) which is the number of workers launched.
  Takes [xs] ([list float]) which holds the input values in order.
  Takes [s] ([msched]) which is the schedule supplied to the model's fixed-tree
  kernel.
*)
Corollary concurrent_replay_can_return workers xs s :
  0 < workers -> exists tail m, rtc (@erased_step replay_language)
    ([replay_sum workers xs], empty_slots)
    (Return (kernel_fixed_tree float PrimFloat.add 0%float s xs) :: tail, m).
Proof.
  destruct workers as [| extra].
  { lia. }
  intros _.
  unfold replay_sum.
  set (gs := assign _ _).
  destruct (finish_groups gs 0 0%float [] empty_slots) as (tail & m & H).

  exists tail, m.
  eapply rtc_l.
  - exists [].
    eapply step_atomic with
      (t1 := []) (t2 := []) (efs := workers_from 0 gs)
      (e2 := Join 0 (length gs) 0%float) (σ2 := empty_slots);
      reflexivity || done.
  - unfold gs in H.
    rewrite assign_concat in H.
    exact H.
Qed.
